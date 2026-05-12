#include "ml/inference.h"
#include "ml/road_quality_model_data.h"

#include <math.h>

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

QueueHandle_t mlQueue = nullptr;

namespace {

constexpr int WINDOW_SIZE = 64;
constexpr int FEATURE_COUNT = 8;
constexpr int STRIDE = 16;
constexpr int NUM_CLASSES = 5;
constexpr int TENSOR_ARENA_SIZE = 32 * 1024;

alignas(16) uint8_t tensor_arena[TENSOR_ARENA_SIZE];

tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;
static tflite::MicroErrorReporter error_reporter;

float ringBuffer[WINDOW_SIZE][FEATURE_COUNT];
float orderedWindow[WINDOW_SIZE * FEATURE_COUNT];

size_t writeIndex = 0;
size_t totalSamples = 0;
size_t samplesSinceLastInference = 0;

volatile float lastRoadQuality = 3.0f;
volatile int lastRoadClass = 3;

// IMPORTANT:
// Replace these with the exact values from training/normalization.json
constexpr float FEATURE_MEAN[FEATURE_COUNT] = {
    -0.5268345475196838,
    1.9714030027389526,
    9.360634803771973,
    16.64743423461914,
    0.011532701551914215,
    0.0008364409441128373,
    0.0008769979467615485,
    21.203208923339844
};

constexpr float FEATURE_STD[FEATURE_COUNT] = {
    2.3695430755615234,
    3.2550084590911865,
    2.7624292373657227,
    1.9307798147201538,
    0.33568328619003296,
    0.3247920870780945,
    0.5829450488090515,
    10.217056274414062
};

float clipf(float x, float minv, float maxv) {
    if (x < minv) return minv;
    if (x > maxv) return maxv;
    return x;
}

void sampleToFeatures(const mpu_data_t& s, float* feat) {
    feat[0] = s.ax;
    feat[1] = s.ay;
    feat[2] = s.az;
    feat[3] = s.gx;
    feat[4] = s.gy;
    feat[5] = s.gz;
    feat[6] = s.temp;
    //feat[7] = clipf(s.vel, 0.0f, 40.0f); TEMP CHANGE FOR TESTING WITHOUT VELOCITY SENSOR, JUST SET TO A CONSTANT SAFE VALUE
    feat[7] = 15.0f;
}

void pushSample(const float* feat) {
    for (int i = 0; i < FEATURE_COUNT; ++i) {
        ringBuffer[writeIndex][i] = feat[i];
    }

    writeIndex = (writeIndex + 1) % WINDOW_SIZE;
    totalSamples++;
    samplesSinceLastInference++;
}

void buildOrderedNormalizedWindow() {
    size_t start = writeIndex;
    int k = 0;

    for (int i = 0; i < WINDOW_SIZE; ++i) {
        size_t idx = (start + i) % WINDOW_SIZE;
        for (int j = 0; j < FEATURE_COUNT; ++j) {
            float x = ringBuffer[idx][j];
            orderedWindow[k++] = (x - FEATURE_MEAN[j]) / (FEATURE_STD[j] + 1e-6f);
        }
    }
}

void fillInputTensor() {
    if (input->type == kTfLiteInt8) {
        const float scale = input->params.scale;
        const int zero_point = input->params.zero_point;

        for (int i = 0; i < WINDOW_SIZE * FEATURE_COUNT; ++i) {
            int32_t q = (int32_t)lrintf(orderedWindow[i] / scale) + zero_point;
            if (q < -128) q = -128;
            if (q > 127) q = 127;
            input->data.int8[i] = (int8_t)q;
        }
    } else if (input->type == kTfLiteFloat32) {
        for (int i = 0; i < WINDOW_SIZE * FEATURE_COUNT; ++i) {
            input->data.f[i] = orderedWindow[i];
        }
    }
}

void readOutputProbabilities(float* probs) {
    if (output->type == kTfLiteInt8) {
        const float scale = output->params.scale;
        const int zero_point = output->params.zero_point;

        for (int i = 0; i < NUM_CLASSES; ++i) {
            probs[i] = (output->data.int8[i] - zero_point) * scale;
            if (probs[i] < 0.0f) probs[i] = 0.0f;
        }
    } else if (output->type == kTfLiteFloat32) {
        for (int i = 0; i < NUM_CLASSES; ++i) {
            probs[i] = output->data.f[i];
            if (probs[i] < 0.0f) probs[i] = 0.0f;
        }
    }

    float sum = 0.0f;
    for (int i = 0; i < NUM_CLASSES; ++i) sum += probs[i];

    if (sum > 1e-6f) {
        for (int i = 0; i < NUM_CLASSES; ++i) probs[i] /= sum;
    }
}

void updatePrediction(const float* probs) {
    float expectedScore = 0.0f;
    int bestIdx = 0;
    float bestProb = probs[0];

    for (int i = 0; i < NUM_CLASSES; ++i) {
        expectedScore += probs[i] * (float)(i + 1);
        if (probs[i] > bestProb) {
            bestProb = probs[i];
            bestIdx = i;
        }
    }

    lastRoadQuality = 0.85f * lastRoadQuality + 0.15f * expectedScore;
    lastRoadClass = bestIdx + 1;

    Serial.printf("road_quality=%.2f road_class=%d probs=[%.2f %.2f %.2f %.2f %.2f]\n",
                  lastRoadQuality, lastRoadClass,
                  probs[0], probs[1], probs[2], probs[3], probs[4]);
}

void InferenceTask(void* parameter) {
    mpu_data_t sample;
    float feat[FEATURE_COUNT];
    float probs[NUM_CLASSES];

    for (;;) {
        if (xQueueReceive(mlQueue, &sample, portMAX_DELAY) == pdPASS) {
            sampleToFeatures(sample, feat);
            pushSample(feat);

            if (totalSamples < WINDOW_SIZE) continue;
            if (samplesSinceLastInference < STRIDE) continue;

            samplesSinceLastInference = 0;

            buildOrderedNormalizedWindow();
            fillInputTensor();

            if (interpreter->Invoke() != kTfLiteOk) {
                Serial.println("TFLM invoke failed");
                continue;
            }

            readOutputProbabilities(probs);
            updatePrediction(probs);
        }
    }
}

}  // namespace

float getLastRoadQuality() {
    return lastRoadQuality;
}

int getLastRoadClass() {
    return lastRoadClass;
}

void setupML() {
    Serial.println("Initializing road quality CNN...");

    const tflite::Model* model = tflite::GetModel(road_quality_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.println("Model schema mismatch!");
        while (1) { delay(1000); }
    }

    static tflite::AllOpsResolver resolver;
    static tflite::MicroInterpreter static_interpreter(
        model,
        resolver,
        tensor_arena,
        sizeof(tensor_arena),
        &error_reporter
    );

    interpreter = &static_interpreter;

    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        Serial.println("AllocateTensors() failed! Increase tensor arena.");
        while (1) { delay(1000); }
    }

    input = interpreter->input(0);
    output = interpreter->output(0);

    xTaskCreatePinnedToCore(InferenceTask, "ML_Inference", 8192, NULL, 1, NULL, 1);
}