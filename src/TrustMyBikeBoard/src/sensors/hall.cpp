#include "sensors/hall.h"

HallSensor* HallSensor::instance = nullptr;

HallSensor::HallSensor(int pin, int distance, int n_magnets)
    : pin(pin), distance(distance), n_magnets(n_magnets),
      tachTaskHandle(NULL), lastPulseTime_us(0),
      currentPulseTime_us(0), frequency(0.0f), frequencyMutex(NULL)
{
    instance = this;
    pulseTimeMutex = portMUX_INITIALIZER_UNLOCKED;
    
    /* Mutex must exist before the task or ISR can use it. */
    frequencyMutex = xSemaphoreCreateMutex();
    configASSERT(frequencyMutex != NULL);

    /* Create the tachometer task BEFORE attaching the ISR so that
     * tachTaskHandle is valid when the first interrupt fires.
     *   Stack   : 4096 words
     *   Priority: 3 */
    BaseType_t result = xTaskCreate(
        hallTask,
        "Tachometer",
        4096,
        (void*)this,
        3,
        &tachTaskHandle
    );
    configASSERT(result == pdPASS);
    configASSERT(tachTaskHandle != NULL);

    /* Attach ISR last — everything it touches is now initialised. */
    pinMode(pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pin),
                    hallISR,
                    FALLING);
}

void HallSensor::setFrequency(float value)
{
    if (xSemaphoreTake(frequencyMutex, portMAX_DELAY) == pdTRUE)
    {
        frequency = value;
        xSemaphoreGive(frequencyMutex);
    }
}

IRAM_ATTR void HallSensor::hallISR()
{
    if (instance == nullptr) return;
    
    uint32_t now = (uint32_t)esp_timer_get_time();

    if (now - instance->currentPulseTime_us < 5000) { // 5ms threshold (tune this)
        return;
    }

    portENTER_CRITICAL_ISR(&instance->pulseTimeMutex);
    instance->lastPulseTime_us = instance->currentPulseTime_us;
    instance->currentPulseTime_us = now;
    portEXIT_CRITICAL_ISR(&instance->pulseTimeMutex);
    /* Wake the tachometer task. Higher-priority tasks preempt immediately. */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(instance->tachTaskHandle, &xHigherPriorityTaskWoken);

    /* Yield to the notified task if it has higher priority than the
     * interrupted task (required by FreeRTOS port). */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void HallSensor::hallTask(void* params)
{
    HallSensor* self = (HallSensor*)params;
    if (self == nullptr) return;
    
    bool firstPulse = true;

    for (;;)
    {
        /* Block until the ISR sends a notification (no timeout = wait forever).
         * ulTaskNotifyTake clears the notification count after returning. */
        int res = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
        if(res == pdFALSE){
            firstPulse = true;
            self->setFrequency(0.0f);
            //Serial.println("identified a first pulse");
        }

        /* --- Snapshot timestamps atomically --- */
        portENTER_CRITICAL(&self->pulseTimeMutex);
        uint32_t t_prev = self->lastPulseTime_us;
        uint32_t t_now  = self->currentPulseTime_us;
        portEXIT_CRITICAL(&self->pulseTimeMutex);

        /* Discard the first pulse: we need two timestamps for a period. */
        if (firstPulse)
        {
            firstPulse = false;

            continue;
        }
        Serial.println("nice");
        /* --- Compute period, guarding against micros() overflow wrap --- */
        uint32_t period_us = (t_now >= t_prev)
                             ? (t_now - t_prev)
                             : (0xFFFFFFFFUL - t_prev + t_now + 1UL);

        /* Avoid division by zero (shouldn't happen, but be safe). */
        if (period_us == 0)
        {
            continue;
        }

        /* --- frequency = 1 000 000 / (period_us × MAGNETS_PER_REV) --- */
        float freq = 1000000.0f / ((float)period_us * (float)self->n_magnets);
        
        Serial.printf("calculated frequency: %f Hz\n", freq);
        /* --- Update shared variable --- */
        self->setFrequency(freq);
    }
}

double HallSensor::getFrequency()
{
    float frequencyCopy = 0.0f;

    if (xSemaphoreTake(frequencyMutex, portMAX_DELAY) == pdTRUE)
    {
        frequencyCopy = frequency;
        xSemaphoreGive(frequencyMutex);
    }

    return frequencyCopy;
}

void HallSensor::setRadius(int radius)
{
    distance = radius;
}

double HallSensor::getSpeed()
{
    float frequencyCopy = 0.0f;

    if (xSemaphoreTake(frequencyMutex, portMAX_DELAY) == pdTRUE)
    {
        frequencyCopy = frequency;
        xSemaphoreGive(frequencyMutex);
    }

    /* speed = frequency (rev/s) × circumference (2π × radius in meters) */
    /* Assuming distance is in mm, convert to meters */
    double circumference = 2.0 * M_PI * (distance / 1000.0);
    return frequencyCopy * circumference;
}