#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern QueueHandle_t mlQueue;

void setupML(); 
void InferenceTask(void *parameter);