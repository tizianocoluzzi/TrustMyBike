#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "sensors/mpu.h"

constexpr int ML_WINDOW_SIZE = 192;
constexpr int ML_STRIDE = 16;
constexpr int ML_NUM_CLASSES = 5;
constexpr int ML_MOTION_FEATURE_COUNT = 6;
constexpr int ML_VEL_FEATURE_COUNT = 4;

typedef struct
{
    mpu_data_t mpu;
    float vel;
} ml_sample_t;

extern QueueHandle_t mlQueue;

void setupML();
float getLastRoadQuality();
int getLastRoadClass();