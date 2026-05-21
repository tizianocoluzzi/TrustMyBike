#include "ml/inference.h"
#include "ml/road_quality_model_data.h"

#include <math.h>
#include <string.h>

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

QueueHandle_t mlQueue = nullptr;

namespace
{

constexpr int WINDOW_SIZE = 64;
constexpr int MOTION_FEATURE_COUNT = 6;   // ax ay az gx gy gz
constexpr int VEL_FEATURE_COUNT = 4;      // mean std last zero_ratio
constexpr int STRIDE = 16;
constexpr int NUM_CLASSES = 5;
constexpr int TENSOR_ARENA_SIZE = 48 * 1024;

// IMPORTANT:
// Replace these values with the exact values from training/normalization.json
constexpr float MOTION_MEAN[MOTION_FEATURE_COUNT] = {
    -0.5268345475196838,
    1.9714030027389526,
    9.360634803771973,
    16.64743423461914,
    0.011532701551914215,
    0.0008364409441128373
};

constexpr float MOTION_STD[MOTION_FEATURE_COUNT] = {
    2.3695430755615234,
    3.2550084590911865,
    2.7624292373657227,
    1.9307798147201538,
    0.33568328619003296,
    0.3247920870780945
};

constexpr float VEL_MEAN[VEL_FEATURE_COUNT] = {
    21.205150604248047,
    2.6830883026123047,
    21.373544692993164,
    0.04279769957065582
};

constexpr float VEL_STD[VEL_FEATURE_COUNT] = {
    9.194299697875977,
    3.5567514896392822,
    10.127305030822754,
    0.19409224390983582
};

alignas(16) uint8_t tensor_arena[TENSOR_ARENA_SIZE];

tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *motion_input = nullptr;
TfLiteTensor *vel_input = nullptr;
TfLiteTensor *output = nullptr;
static tflite::MicroErrorReporter error_reporter;

float motionRing[WINDOW_SIZE][MOTION_FEATURE_COUNT];
float velRing[WINDOW_SIZE];

float orderedMotionWindow[WINDOW_SIZE * MOTION_FEATURE_COUNT];
float normalizedVelFeatures[VEL_FEATURE_COUNT];

size_t writeIndex = 0;
size_t totalSamples = 0;
size_t samplesSinceLastInference = 0;

volatile float lastRoadQuality = 3.0f;
volatile int lastRoadClass = 3;

inline float clipf(float x, float minv, float maxv)
{
    if (x < minv)
        return minv;
    if (x > maxv)
        return maxv;
    return x;
}

void sampleToMotionFeatures(const mpu_data_t &s, float *feat)
{
    feat[0] = s.ax;
    feat[1] = s.ay;
    feat[2] = s.az;
    feat[3] = s.gx;
    feat[4] = s.gy;
    feat[5] = s.gz;
}

void pushSample(const ml_sample_t &s)
{
    float feat[MOTION_FEATURE_COUNT];
    sampleToMotionFeatures(s.mpu, feat);

    for (int i = 0; i < MOTION_FEATURE_COUNT; ++i)
    {
        motionRing[writeIndex][i] = feat[i];
    }

    velRing[writeIndex] = clipf(s.vel, 0.0f, 40.0f);

    writeIndex = (writeIndex + 1) % WINDOW_SIZE;
    totalSamples++;
    samplesSinceLastInference++;
}

void buildOrderedNormalizedMotionWindow()
{
    size_t start = writeIndex;
    int k = 0;

    for (int i = 0; i < WINDOW_SIZE; ++i)
    {
        size_t idx = (start + i) % WINDOW_SIZE;
        for (int j = 0; j < MOTION_FEATURE_COUNT; ++j)
        {
            float x = motionRing[idx][j];
            orderedMotionWindow[k++] = (x - MOTION_MEAN[j]) / (MOTION_STD[j] + 1e-6f);
        }
    }
}

void buildNormalizedVelocityFeatures()
{
    size_t start = writeIndex;
    float sum = 0.0f;
    float sq_sum = 0.0f;
    float last_vel = 0.0f;
    int zero_count = 0;

    for (int i = 0; i < WINDOW_SIZE; ++i)
    {
        size_t idx = (start + i) % WINDOW_SIZE;
        float v = velRing[idx];
        sum += v;
        sq_sum += v * v;

        if (v <= 1e-3f)
            zero_count++;

        if (i == WINDOW_SIZE - 1)
            last_vel = v;
    }

    const float mean = sum / WINDOW_SIZE;
    float var = (sq_sum / WINDOW_SIZE) - (mean * mean);
    if (var < 0.0f)
        var = 0.0f;
    const float std = sqrtf(var);
    const float zero_ratio = (float)zero_count / (float)WINDOW_SIZE;

    float raw[VEL_FEATURE_COUNT] = {mean, std, last_vel, zero_ratio};

    for (int i = 0; i < VEL_FEATURE_COUNT; ++i)
    {
        normalizedVelFeatures[i] = (raw[i] - VEL_MEAN[i]) / (VEL_STD[i] + 1e-6f);
    }
}

void fillOneInputTensor(TfLiteTensor *tensor, const float *src, int count)
{
    if (tensor->type == kTfLiteInt8)
    {
        const float scale = tensor->params.scale;
        const int zero_point = tensor->params.zero_point;

        for (int i = 0; i < count; ++i)
        {
            int32_t q = (int32_t)lrintf(src[i] / scale) + zero_point;
            if (q < -128)
                q = -128;
            if (q > 127)
                q = 127;
            tensor->data.int8[i] = (int8_t)q;
        }
    }
    else if (tensor->type == kTfLiteFloat32)
    {
        for (int i = 0; i < count; ++i)
        {
            tensor->data.f[i] = src[i];
        }
    }
}

bool tensorLooksLikeMotionInput(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->dims &&
           tensor->dims->size == 3 &&
           tensor->dims->data[1] == WINDOW_SIZE &&
           tensor->dims->data[2] == MOTION_FEATURE_COUNT;
}

bool tensorLooksLikeVelocityInput(const TfLiteTensor *tensor)
{
    return tensor &&
           tensor->dims &&
           tensor->dims->size == 2 &&
           tensor->dims->data[1] == VEL_FEATURE_COUNT;
}

void resolveModelInputs()
{
    TfLiteTensor *in0 = interpreter->input(0);
    TfLiteTensor *in1 = interpreter->input(1);

    if (tensorLooksLikeMotionInput(in0) && tensorLooksLikeVelocityInput(in1))
    {
        motion_input = in0;
        vel_input = in1;
        return;
    }

    if (tensorLooksLikeMotionInput(in1) && tensorLooksLikeVelocityInput(in0))
    {
        motion_input = in1;
        vel_input = in0;
        return;
    }

    Serial.println("Could not resolve model input order");
    while (1)
    {
        delay(1000);
    }
}

void readOutputProbabilities(float *probs)
{
    if (output->type == kTfLiteInt8)
    {
        const float scale = output->params.scale;
        const int zero_point = output->params.zero_point;

        for (int i = 0; i < NUM_CLASSES; ++i)
        {
            probs[i] = (output->data.int8[i] - zero_point) * scale;
            if (probs[i] < 0.0f)
                probs[i] = 0.0f;
        }
    }
    else if (output->type == kTfLiteFloat32)
    {
        for (int i = 0; i < NUM_CLASSES; ++i)
        {
            probs[i] = output->data.f[i];
            if (probs[i] < 0.0f)
                probs[i] = 0.0f;
        }
    }

    float sum = 0.0f;
    for (int i = 0; i < NUM_CLASSES; ++i)
        sum += probs[i];

    if (sum > 1e-6f)
    {
        for (int i = 0; i < NUM_CLASSES; ++i)
            probs[i] /= sum;
    }
}

void updatePrediction(const float *probs)
{
    float expectedScore = 0.0f;
    int bestIdx = 0;
    float bestProb = probs[0];

    for (int i = 0; i < NUM_CLASSES; ++i)
    {
        expectedScore += probs[i] * (float)(i + 1);
        if (probs[i] > bestProb)
        {
            bestProb = probs[i];
            bestIdx = i;
        }
    }

    lastRoadQuality = 0.85f * lastRoadQuality + 0.15f * expectedScore;
    lastRoadClass = bestIdx + 1;
    g_road_class_display = lastRoadClass;

    Serial.printf(
        "road_quality=%.2f road_class=%d probs=[%.2f %.2f %.2f %.2f %.2f]\n",
        lastRoadQuality, lastRoadClass,
        probs[0], probs[1], probs[2], probs[3], probs[4]);
}

void InferenceTask(void *parameter)
{
    ml_sample_t sample;
    float probs[NUM_CLASSES];

    for (;;)
    {
        if (xQueueReceive(mlQueue, &sample, portMAX_DELAY) == pdPASS)
        {
            pushSample(sample);

            if (totalSamples < WINDOW_SIZE)
                continue;
            if (samplesSinceLastInference < STRIDE)
                continue;

            samplesSinceLastInference = 0;

            buildOrderedNormalizedMotionWindow();
            buildNormalizedVelocityFeatures();

            fillOneInputTensor(motion_input, orderedMotionWindow, WINDOW_SIZE * MOTION_FEATURE_COUNT);
            fillOneInputTensor(vel_input, normalizedVelFeatures, VEL_FEATURE_COUNT);

            if (interpreter->Invoke() != kTfLiteOk)
            {
                Serial.println("TFLM invoke failed");
                continue;
            }

            readOutputProbabilities(probs);
            updatePrediction(probs);
        }
    }
}

} // namespace

volatile int g_road_class_display = 3;

float getLastRoadQuality()
{
    return lastRoadQuality;
}

int getLastRoadClass()
{
    return lastRoadClass;
}

void setupML()
{
    Serial.println("Initializing road quality fusion model...");

    const tflite::Model *model = tflite::GetModel(road_quality_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        Serial.println("Model schema mismatch!");
        while (1)
        {
            delay(1000);
        }
    }

    static tflite::AllOpsResolver resolver;
    static tflite::MicroInterpreter static_interpreter(
        model,
        resolver,
        tensor_arena,
        sizeof(tensor_arena),
        &error_reporter);

    interpreter = &static_interpreter;

    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk)
    {
        Serial.println("AllocateTensors() failed! Increase tensor arena.");
        while (1)
        {
            delay(1000);
        }
    }

    resolveModelInputs();

    output = interpreter->output(0);

    Serial.printf("Input0 dims: ");
    for (int i = 0; i < interpreter->input(0)->dims->size; ++i)
        Serial.printf("%d ", interpreter->input(0)->dims->data[i]);
    Serial.println();

    Serial.printf("Input1 dims: ");
    for (int i = 0; i < interpreter->input(1)->dims->size; ++i)
        Serial.printf("%d ", interpreter->input(1)->dims->data[i]);
    Serial.println();

    xTaskCreatePinnedToCore(InferenceTask, "ML_Inference", 8192, NULL, 1, NULL, 1);
}