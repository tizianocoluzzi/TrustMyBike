#include "ml/inference.h"
#include "ml/road_quality_model_data.h"

#include <math.h>
#include <string.h>

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

QueueHandle_t mlQueue = nullptr;

namespace {

constexpr int WINDOW_SIZE = ML_WINDOW_SIZE;
constexpr int MOTION_FEATURE_COUNT = ML_MOTION_FEATURE_COUNT;
constexpr int VEL_FEATURE_COUNT = ML_VEL_FEATURE_COUNT;
constexpr int STRIDE = ML_STRIDE;
constexpr int NUM_CLASSES = ML_NUM_CLASSES;
constexpr int TENSOR_ARENA_SIZE = 40 * 1024;

alignas(16) uint8_t tensor_arena[TENSOR_ARENA_SIZE];

tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input_motion = nullptr;
TfLiteTensor* input_vel = nullptr;
TfLiteTensor* output = nullptr;
static tflite::MicroErrorReporter error_reporter;

float motionRingBuffer[WINDOW_SIZE][MOTION_FEATURE_COUNT];
float velRingBuffer[WINDOW_SIZE];

float orderedMotionWindow[WINDOW_SIZE * MOTION_FEATURE_COUNT];
float velFeatureVector[VEL_FEATURE_COUNT];

size_t writeIndex = 0;
size_t totalSamples = 0;
size_t samplesSinceLastInference = 0;

volatile float lastRoadQuality = 3.0f;
volatile int lastRoadClass = 3;

// normalization.json
constexpr float MOTION_MEAN[MOTION_FEATURE_COUNT] = {
    -0.5196636915206909f,
    1.9343878030776978f,
    9.374368667602539f,
    16.346221923828125f,
    0.020852921530604362f,
    0.0009348283056169748f
};

constexpr float MOTION_STD[MOTION_FEATURE_COUNT] = {
    2.4927282333374023f,
    3.5587306022644043f,
    2.9328017234802246f,
    1.8535943031311035f,
    0.2886029779911041f,
    0.2796790599822998f
};

constexpr float VEL_MEAN[VEL_FEATURE_COUNT] = {
    20.4254093170166f,
    3.884416103363037f,
    20.970443725585938f,
    0.04338475316762924f
};

constexpr float VEL_STD[VEL_FEATURE_COUNT] = {
    8.363607406616211f,
    4.269894599914551f,
    9.950922012329102f,
    0.18576408922672272f
};

float clipf(float x, float minv, float maxv) {
    if (x < minv) return minv;
    if (x > maxv) return maxv;
    return x;
}

void sampleToMotionFeatures(const mpu_data_t& s, float* feat) {
    feat[0] = s.ax;
    feat[1] = s.ay;
    feat[2] = s.az;
    feat[3] = s.gx;
    feat[4] = s.gy;
    feat[5] = s.gz;
}

void pushSample(const float* motionFeat, float vel) {
    for (int i = 0; i < MOTION_FEATURE_COUNT; ++i) {
        motionRingBuffer[writeIndex][i] = motionFeat[i];
    }
    velRingBuffer[writeIndex] = vel;

    writeIndex = (writeIndex + 1) % WINDOW_SIZE;
    totalSamples++;
    samplesSinceLastInference++;
}

void buildOrderedNormalizedMotionWindow() {
    size_t start = writeIndex;
    int k = 0;

    for (int i = 0; i < WINDOW_SIZE; ++i) {
        size_t idx = (start + i) % WINDOW_SIZE;
        for (int j = 0; j < MOTION_FEATURE_COUNT; ++j) {
            float x = motionRingBuffer[idx][j];
            orderedMotionWindow[k++] = (x - MOTION_MEAN[j]) / (MOTION_STD[j] + 1e-6f);
        }
    }
}

void buildVelocityFeatures() {
    size_t start = writeIndex;

    float vel_sum = 0.0f;
    float vel_sq_sum = 0.0f;
    int zero_count = 0;

    for (int i = 0; i < WINDOW_SIZE; ++i) {
        size_t idx = (start + i) % WINDOW_SIZE;
        float v = velRingBuffer[idx];
        vel_sum += v;
        vel_sq_sum += v * v;
        if (v <= 1e-3f) zero_count++;
    }

    float vel_mean = vel_sum / WINDOW_SIZE;
    float variance = (vel_sq_sum / WINDOW_SIZE) - (vel_mean * vel_mean);
    if (variance < 0.0f) variance = 0.0f;
    float vel_std = sqrtf(variance);
    float vel_last = velRingBuffer[(writeIndex + WINDOW_SIZE - 1) % WINDOW_SIZE];
    float vel_zero_ratio = static_cast<float>(zero_count) / WINDOW_SIZE;

    float raw[VEL_FEATURE_COUNT] = {
        vel_mean,
        vel_std,
        vel_last,
        vel_zero_ratio
    };

    for (int i = 0; i < VEL_FEATURE_COUNT; ++i) {
        velFeatureVector[i] = (raw[i] - VEL_MEAN[i]) / (VEL_STD[i] + 1e-6f);
    }
}

void fillInputTensors() {
    if (input_motion->type == kTfLiteInt8) {
        const float scale = input_motion->params.scale;
        const int zero_point = input_motion->params.zero_point;

        for (int i = 0; i < WINDOW_SIZE * MOTION_FEATURE_COUNT; ++i) {
            int32_t q = (int32_t)lrintf(orderedMotionWindow[i] / scale) + zero_point;
            if (q < -128) q = -128;
            if (q > 127) q = 127;
            input_motion->data.int8[i] = (int8_t)q;
        }
    } else if (input_motion->type == kTfLiteFloat32) {
        for (int i = 0; i < WINDOW_SIZE * MOTION_FEATURE_COUNT; ++i) {
            input_motion->data.f[i] = orderedMotionWindow[i];
        }
    }

    if (input_vel->type == kTfLiteInt8) {
        const float scale = input_vel->params.scale;
        const int zero_point = input_vel->params.zero_point;

        for (int i = 0; i < VEL_FEATURE_COUNT; ++i) {
            int32_t q = (int32_t)lrintf(velFeatureVector[i] / scale) + zero_point;
            if (q < -128) q = -128;
            if (q > 127) q = 127;
            input_vel->data.int8[i] = (int8_t)q;
        }
    } else if (input_vel->type == kTfLiteFloat32) {
        for (int i = 0; i < VEL_FEATURE_COUNT; ++i) {
            input_vel->data.f[i] = velFeatureVector[i];
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

    Serial.printf(
        "road_quality=%.2f road_class=%d probs=[%.2f %.2f %.2f %.2f %.2f]\n",
        lastRoadQuality,
        lastRoadClass,
        probs[0], probs[1], probs[2], probs[3], probs[4]
    );
}

void InferenceTask(void* parameter) {
    ml_sample_t sample;
    float motionFeat[MOTION_FEATURE_COUNT];
    float probs[NUM_CLASSES];

    for (;;) {
        if (xQueueReceive(mlQueue, &sample, portMAX_DELAY) == pdPASS) {
            sampleToMotionFeatures(sample.mpu, motionFeat);
            float vel = clipf(sample.vel, 0.0f, 40.0f);

            pushSample(motionFeat, vel);

            if (totalSamples < WINDOW_SIZE) continue;
            if (samplesSinceLastInference < STRIDE) continue;

            samplesSinceLastInference = 0;

            buildOrderedNormalizedMotionWindow();
            buildVelocityFeatures();
            fillInputTensors();

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
    Serial.println("Initializing road quality fusion model...");

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

    input_motion = interpreter->input(0);
    input_vel = interpreter->input(1);
    output = interpreter->output(0);

    if (!input_motion || !input_vel || !output) {
        Serial.println("Failed to get model tensors");
        while (1) { delay(1000); }
    }

    Serial.print("Input 0 dims: ");
    for (int i = 0; i < input_motion->dims->size; ++i) {
        Serial.printf("%d ", input_motion->dims->data[i]);
    }
    Serial.println();

    Serial.print("Input 1 dims: ");
    for (int i = 0; i < input_vel->dims->size; ++i) {
        Serial.printf("%d ", input_vel->dims->data[i]);
    }
    Serial.println();

    Serial.print("Output dims: ");
    for (int i = 0; i < output->dims->size; ++i) {
        Serial.printf("%d ", output->dims->data[i]);
    }
    Serial.println();

    Serial.printf("Input 0 type=%d scale=%f zp=%d\n",
                  input_motion->type, input_motion->params.scale, input_motion->params.zero_point);
    Serial.printf("Input 1 type=%d scale=%f zp=%d\n",
                  input_vel->type, input_vel->params.scale, input_vel->params.zero_point);
    Serial.printf("Output  type=%d scale=%f zp=%d\n",
                  output->type, output->params.scale, output->params.zero_point);

    xTaskCreatePinnedToCore(InferenceTask, "ML_Inference", 8192, NULL, 1, NULL, 1);
}