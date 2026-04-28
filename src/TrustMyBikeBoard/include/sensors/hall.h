#include "FreeRTOS.h"
#include <task.h>
#include <semphr.h>
#include <Arduino.h>

class HallSensor {
public:
    HallSensor(int pin, int distance, int n_magnets);
    double getFrequency();
    double getSpeed();
    void setRadius(int radius);

private:
    static HallSensor* instance;
    
    int pin;
    int distance;
    int n_magnets;
    
    TaskHandle_t tachTaskHandle;
    volatile uint32_t lastPulseTime_us;
    volatile uint32_t currentPulseTime_us;
    float frequency;
    SemaphoreHandle_t frequencyMutex;
    portMUX_TYPE pulseTimeMutex;
    
    void setFrequency(float value);
    static void hallISR();
    static void hallTask(void* params);
};
