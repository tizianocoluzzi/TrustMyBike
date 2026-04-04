#include "ml/inference.h"
#include "ml/model_data.h"
#include <Arduino.h>

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/schema/schema_generated.h"

const float MIN_ACCEL = 0.0;
const float MAX_ACCEL = 20.0;
const float ANOMALY_THRESHOLD = 0.10; 

alignas(16) uint8_t tensor_arena[8 * 1024];
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;
static tflite::MicroErrorReporter error_reporter;

// Define the queue here
QueueHandle_t mlQueue;

void InferenceTask(void *parameter) {
    float raw_accel_z = 0.0f;

    for (;;) {
        // Wait indefinitely until the filterTask sends a new Z-axis reading
        if (xQueueReceive(mlQueue, &raw_accel_z, portMAX_DELAY) == pdPASS) {
            
            // 1. Normalize
            float normalized_input = (raw_accel_z - MIN_ACCEL) / (MAX_ACCEL - MIN_ACCEL);
            if (normalized_input < 0.0f) normalized_input = 0.0f;
            if (normalized_input > 1.0f) normalized_input = 1.0f;

            // 2. Predict
            input->data.f[0] = normalized_input;
            interpreter->Invoke();
            float normalized_output = output->data.f[0];

            // 3. Calculate Error
            float anomaly_score = abs(normalized_input - normalized_output);

            // 4. Trigger bump detection
            if (anomaly_score > ANOMALY_THRESHOLD) {
                Serial.printf(">>> BUMP DETECTED! Z: %.2f | Score: %.3f <<<\n", raw_accel_z, anomaly_score);
            }
        }
    }
}

void setupML() {
    Serial.println("Initializing ML Model...");

    const tflite::Model* model = tflite::GetModel(autoencoder_model_data);
    static tflite::AllOpsResolver resolver;
    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, sizeof(tensor_arena), &error_reporter);
    
    interpreter = &static_interpreter;
    interpreter->AllocateTensors();

    input = interpreter->input(0);
    output = interpreter->output(0);

    // Create the task (Priority 1 is fine, same as other tasks)
    xTaskCreatePinnedToCore(InferenceTask, "ML_Inference", 8192, NULL, 1, NULL, 1);
}