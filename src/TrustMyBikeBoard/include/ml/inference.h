#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "sensors/mpu.h"

// Matches the new model training setup:
// - motion branch uses ax, ay, az, gx, gy, gz over a 64-sample window
// - velocity branch uses window features derived from vel
constexpr int ML_WINDOW_SIZE = 64;
constexpr int ML_STRIDE = 16;
constexpr int ML_NUM_CLASSES = 5;
constexpr int ML_MOTION_FEATURE_COUNT = 6;
constexpr int ML_VEL_FEATURE_COUNT = 4;

// Queue sample sent from main/gatherTask to inference task
typedef struct
{
    mpu_data_t mpu;
    float vel;
} ml_sample_t;

// Global queue used by main.cpp and inference.cpp
extern QueueHandle_t mlQueue;

// Public API
void setupML();
float getLastRoadQuality();
int getLastRoadClass();