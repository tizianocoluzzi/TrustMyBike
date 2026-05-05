#pragma once

#include <Arduino.h>
#include "sensors/mpu.h"

extern QueueHandle_t mlQueue;

void setupML();
float getLastRoadQuality();
int getLastRoadClass();