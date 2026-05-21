#include <Adafruit_INA219.h>
#include <Arduino.h>
#include <Wire.h>
#ifdef SD
#include "sd/sd.h"
#endif
#include "board.h"
#include "display/display.h"
#include "heltec.h"
#include "ml/inference.h"
//#include "secrets.h" //password and SSID stored
#include "sensors/hall.h"
#include "sensors/mpu.h"
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "driver/rtc_io.h"

// #define TEST_MODE 1
#define WINDOW_SIZE 16
#define SAMPLING_FREQUENCY 50

const uint32_t sampling_interval = 1000 / SAMPLING_FREQUENCY;
const double dt = sampling_interval / 1000.0;

uint32_t start_time;
TaskHandle_t gatherTaskHandle;
TaskHandle_t filterTaskHandle;
TaskHandle_t testMLTaskHandle;

QueueHandle_t filterQueue;
QueueHandle_t bleTxQueue;

HallSensor *hallSensor = nullptr;
BLEServer *pServer = nullptr;
typedef struct{
    mpu_data_t mpu;
    double vel;
    double volt;
} general_data;

#ifdef SD
// Counter-based filename for SD card
char currentDataFile[32] = "/data_0.csv";
#endif

Adafruit_INA219 ina;

// ── UUIDs
// ────────────────────────────────────────────────────────────────────
#define SERVICE_UUID "12345678-1234-1234-1234-123456789abc"
#define CHAR_TX_UUID "12345678-1234-1234-1234-123456789ab0" // Notify → phone
#define CHAR_RX_UUID "12345678-1234-1234-1234-123456789ab1" // Write  ← phone

// ── Globals
// ──────────────────────────────────────────────────────────────────
static BLECharacteristic *pTxChar = nullptr;
static bool deviceConnected = false;
static volatile bool newDataAvailable = false;
static String receivedData = "";
static SemaphoreHandle_t dataMutex;

// ── BLE Callbacks
// ─────────────────────────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks
{
    void
    onConnect(BLEServer *pServer) override
    {
        deviceConnected = true;

        Serial.printf("[BLE] Client connected from start in time: %f ms\n", ((float) esp_timer_get_time()-(float)start_time)/1000);
    }
    void
    onDisconnect(BLEServer *pServer) override
    {
        deviceConnected = false;
        Serial.println("[BLE] Client disconnected — restarting advertising");
        pServer->startAdvertising();
    }
};

class RxCallbacks : public BLECharacteristicCallbacks
{
    void
    onWrite(BLECharacteristic *pChar) override
    {
        String value = pChar->getValue().c_str();
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE)
        {
            receivedData = value;
            newDataAvailable = true;
            xSemaphoreGive(dataMutex);
        }
        Serial.printf("[BLE] Received: %s\n", value.c_str());
    }
};

// ── BLE Init
// ─────────────────────────────────────────────────────────────────
static void
initBLE()
{
    BLEDevice::init("Heltec-V3");

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    // TX characteristic — notify
    pTxChar = pService->createCharacteristic(
        CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pTxChar->addDescriptor(new BLE2902());

    // RX characteristic — write
    BLECharacteristic *pRxChar = pService->createCharacteristic(
        CHAR_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pRxChar->setCallbacks(new RxCallbacks());

    pService->start();

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.println("[BLE] Advertising started");
}

void gatherTask(void *param)
{
    int i = 0;
    general_data data;
    mpu_data_t mpu_data;
    ml_sample_t ml_sample;
    char buf[512];
    int cnt = 0;
    for (;;)
    {
        readAccelGyro(&mpu_data);
        double velocity = hallSensor ? hallSensor->getSpeed() : 0.0;
        memccpy(&data, &mpu_data,1, sizeof(mpu_data_t));
        data.vel = velocity;
        data.volt = ina.getBusVoltage_V();
        snprintf(buf, sizeof(buf), "%f,%f,%f,%f,%f,%f,%f,%f,%f\n", data.mpu.ax,
                 data.mpu.ay, data.mpu.az, data.mpu.temp, data.mpu.gx, data.mpu.gy, data.mpu.gz,
                 data.volt,data.vel);
        // Serial.printf(buf);
        // sd::write_csv(currentDataFile, buf);
        // xQueueSend(filterQueue, &data, portMAX_DELAY);
        ml_sample.mpu = mpu_data;
        ml_sample.vel = (float)velocity;
        xQueueSend(mlQueue, (void*) &ml_sample, portMAX_DELAY); //at the moment just mpu data
        if (i == 0)
        {
            snprintf(buf, sizeof(buf),
                     "volt:%6.2f vel:%6.2f\naz:%6.2f road:%d",
                     data.volt, velocity, data.mpu.az,
                     g_road_class_display);
            display_message(buf);
        }
        i = (i + 1) % 10;
        if(data.vel == 0)
            cnt++;
        else 
            cnt = 0;
        if(cnt >= 1000){ //if stop for 20 second
            Serial.println("[Sleep] Entering deep sleep");

            if (pServer->getConnectedCount() > 0)
            {
                pServer->disconnect(0);
                delay(300); // let disconnect packet transmit
            }

           // gpio_reset_pin(HALL_GPIO);
           // gpio_set_direction(HALL_GPIO, GPIO_MODE_INPUT);
           // gpio_pullup_en(HALL_GPIO);
           // gpio_pulldown_dis(HALL_GPIO);

            uint64_t pinMask = (1ULL << HALL_GPIO);
            esp_sleep_enable_ext1_wakeup(pinMask, ESP_EXT1_WAKEUP_ANY_LOW);

            Serial.println("[Sleep] Going to sleep now — short pin to GND to wake");
            Serial.flush();
            delay(100);
            esp_deep_sleep_start();
        }
        vTaskDelay(pdMS_TO_TICKS(sampling_interval));
    }
}

void filterTask(void *param)
{
    mpu_data_t window[WINDOW_SIZE] = {0};
    mpu_data_t running_sum = {0};
    mpu_data_t data = {0};
    mpu_data_t data_mean = {0};
    int window_count = 0;
    int index = 0;
    double pitch_y = 0.0f;
    double roll_x = 0.0f;
    double yaw_z = 0.0f;
    double pitch_y_mean = 0.0f;
    double roll_x_mean = 0.0f;
    double yaw_z_mean = 0.0f;
    double vel_z = 0.0f;
    double vel_z_mean = 0.0f;
    double pos_z = 0.0f;
    double pos_z_mean = 0.0f;
    for (;;)
    {
        xQueueReceive(filterQueue, &data, portMAX_DELAY);

        if (window_count == WINDOW_SIZE)
        {
            subSample(&running_sum, &window[index]);
        }
        else
        {
            window_count++;
        }

        window[index] = data;
        addSample(&running_sum, &data);
        index = (index + 1) % WINDOW_SIZE;

        divSample(&data_mean, &running_sum, (float)window_count);

        // Serial.printf(">mean_ax: %6.2f,mean_ay: %6.2f,mean_az: %6.2f,
        // mean_temp:%6.2f, mean_gx: %6.2f, mean_gy: %6.2f, mean_gz:%6.2f\r\n",
        //                data_mean.ax, data_mean.ay, data_mean.az,
        //                data_mean.temp, data_mean.gx, data_mean.gy,
        //                data_mean.gz);
        roll_x += ((double)data.gx) * dt;
        pitch_y += ((double)data.gy) * dt;
        yaw_z += ((double)data.gz) * dt;
        roll_x_mean += ((double)data_mean.gx) * dt;
        pitch_y_mean += ((double)data_mean.gy) * dt;
        yaw_z_mean += ((double)data_mean.gz) * dt;

        double gx = 9.81 * sin(pitch_y_mean);
        double gy = 9.81 * cos(pitch_y_mean) * sin(roll_x_mean);
        double gz = 9.81 * cos(pitch_y_mean) * cos(roll_x_mean);
        double decay = 0.995; // to limit drifting (clearly alters data so //TODO
                              // sensor fusion)
        vel_z += ((double)data.az - gz) * dt;
        vel_z_mean = vel_z_mean * decay + ((double)data_mean.az - gz) * dt;
        pos_z += ((double)data.az - gz) * dt * dt * 0.5 + vel_z * dt;
        pos_z_mean += ((double)data_mean.az - gz) * dt * dt * 0.5 + vel_z_mean * dt;
        // Serial.printf(">roll_x:%f,roll_x_mean:%f,pitch_y:%f,pitch_y_mean:%f,yaw_z:%f,yaw_z_mean:%f,vel_z:%f,vel_z_mean:%f,pos_z:%f,pos_z_mean:%f\r\n",
        //                roll_x, roll_x_mean,
        //                pitch_y, pitch_y_mean,
        //                yaw_z, yaw_z_mean,
        //                vel_z, vel_z_mean,
        //                pos_z, pos_z_mean);
        //  Serial.printf(">az:%f, gz_est:%f, diff:%f\r\n",data_mean.az, gz,
        //  data_mean.az - gz);

        // Not needed anymore, directly sending raw data to ML task
        // float z_to_send = data.az;
        // xQueueSend(mlQueue, &z_to_send, 0);
    }
}
#ifdef TEST_MODE
void testMLTask(void *param)
{
    mpu_data_t sample = {0};
    ml_sample_t ml_sample = {0};
    TickType_t lastWake = xTaskGetTickCount();

    for (;;)
    {
        Serial.println("--- OFF ROAD ---");

        for (int i = 0; i < 200; i++)
        {
            sample.ax = 0.15f + 0.04f * sinf(i * 0.20f);
            sample.ay = 0.40f + 0.06f * cosf(i * 0.18f);
            sample.az = 9.70f + 0.08f * sinf(i * 0.22f);
            sample.gx = 16.60f + 0.05f * sinf(i * 0.10f);
            sample.gy = 0.01f * sinf(i * 0.15f);
            sample.gz = 0.01f * cosf(i * 0.16f);
            sample.temp = 0.0f;


            ml_sample.mpu = sample;
            ml_sample.vel = 15.0f;
            xQueueSend(mlQueue, &ml_sample, portMAX_DELAY);
            vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
        }

        Serial.println("--- NORMAL ROAD ---");

        for (int i = 0; i < 200; i++)
        {
            sample.ax = -0.8f + 1.2f * sinf(i * 0.23f) + 0.9f * cosf(i * 0.41f);
            sample.ay = 2.8f + 1.0f * sinf(i * 0.19f) + 1.1f * cosf(i * 0.33f);
            sample.az = 9.3f + 0.9f * sinf(i * 0.27f) + 1.0f * cosf(i * 0.37f);
            sample.gx = 18.8f + 0.35f * sinf(i * 0.11f) + 0.20f * cosf(i * 0.29f);
            sample.gy = 0.03f + 0.10f * sinf(i * 0.21f) + 0.06f * cosf(i * 0.31f);
            sample.gz = 0.02f + 0.22f * sinf(i * 0.17f) - 0.18f * cosf(i * 0.26f);
            sample.temp = 0.0f;

            if ((i % 37) == 0)
            {
                sample.ax -= 2.5f;
                sample.ay += 1.8f;
                sample.az += 2.8f;
                sample.gz -= 0.35f;
            }

            if ((i % 53) == 0)
            {
                sample.ax += 3.0f;
                sample.az -= 1.8f;
                sample.gy += 0.18f;
            }


            ml_sample.mpu = sample;
            ml_sample.vel = 15.0f;
            xQueueSend(mlQueue, &ml_sample, portMAX_DELAY);
            vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
        }
    }
}
#endif //TEST_MODE
// Task 1 — handles incoming BLE data
void taskBLERx(void *pvParameters)
{
    for (;;)
    {
        if (newDataAvailable)
        {
            String data;
            if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE)
            {
                data = receivedData;
                newDataAvailable = false;
                xSemaphoreGive(dataMutex);
            }
            // Process received data here
            Serial.printf("[RX Task] Processing: %s\n", data.c_str());
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Task 2 — sends periodic notifications to connected client
void taskBLETx(void *pvParameters)
{
    for (;;)
    {
        if (deviceConnected && pTxChar)
        {
            int score = getLastRoadClass();   // current class: 1..5
            if (score < 1) score = 1;
            if (score > 5) score = 5;

            String msg = String(score);
            pTxChar->setValue(msg.c_str());
            pTxChar->notify();
            Serial.printf("[TX Task] Sent road quality: %s\n", msg.c_str());
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // send every 2 s
    }
}

void setup()
{
    Heltec.begin(true, false, true);
    Serial.begin(115200);
    while (!Serial)
        delay(10);
    start_time = esp_timer_get_time();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT1)
    { // note: EXT1 not EXT0
        uint64_t wakePin = esp_sleep_get_ext1_wakeup_status();
        Serial.printf("[Wake] EXT1 triggered, pin mask: %llu\n", wakePin);
    }
#ifndef TEST_MODE //if in test mode sensors are not initialized
    Wire1.begin(SDA_PIN, SCL_PIN);
    Wire1.setClock(100000);
    ina.begin(&Wire1);
    ina.setCalibration_32V_2A();
    mpu_setup();
    display_message("loading calibration");

    // Try to load calibration data from Preferences
    if (!loadCalibrationFromPreferences())
    {
        // If load fails, perform calibration
        display_message("calibrating mpu");
        calibrateMPU();
    }

    // Initialize hall sensor with 1 magnet and 15cm distance
    hallSensor = new HallSensor(HALL_GPIO, 221, 1); // GPIO 33, 150mm (15cm), 1 magnet
#endif
#ifdef SD //SD is for offline data gathering, not the project purpose, just for development
    sd::init();

    // Read counter from SD card and create new filename for this boot
    uint32_t fileCounter = sd::read_counter();
    snprintf(currentDataFile, sizeof(currentDataFile), "/data_%lu.csv",
             fileCounter);
    Serial.printf("Current session data file: %s\n", currentDataFile);

    // Increment counter for next boot
    sd::increment_counter();
    sd::write_csv(currentDataFile, "ax,ay,az,gx,gy,gz,temp,volt,curr,vel\n");
#endif

    filterQueue = xQueueCreate(WINDOW_SIZE * 2, sizeof(mpu_data_t));
    mlQueue = xQueueCreate(64, sizeof(ml_sample_t));
    bleTxQueue = xQueueCreate(10, sizeof(mpu_data_t)); //TODO to be connected to the bletx task and to decide which data format to send
    dataMutex = xSemaphoreCreateMutex();
    configASSERT(dataMutex);

    initBLE();

    setupML();

#ifdef TEST_MODE //creates just the testMLTask in case of test(data gen)
    xTaskCreatePinnedToCore(testMLTask, "testMLTask", 4096, NULL, 1,
                            &testMLTaskHandle, 1);
#else
    xTaskCreatePinnedToCore(gatherTask, "gatherTask", 4096, NULL, 1,
                            &gatherTaskHandle, 1);
    // xTaskCreatePinnedToCore(filterTask, "filterTask", 4096, NULL, 1,
    // &filterTaskHandle, 1);
#endif
    xTaskCreatePinnedToCore(taskBLERx, "BLE_RX", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskBLETx, "BLE_TX", 4096, nullptr, 1, nullptr, 1);
}

void loop()
{
    while (1)
    {
        delay(1000);
    }
}