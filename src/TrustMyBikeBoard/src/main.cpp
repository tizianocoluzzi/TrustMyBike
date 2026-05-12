#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_INA219.h>
#include "sd/sd.h"
#include "heltec.h"
#include "board.h"
#include "sensors/mpu.h"
#include "ml/inference.h"
#include "display/display.h"
#include "sensors/hall.h"
#include "secrets.h" //password and SSID stored
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#ifndef WIFI_SSID
//#error "ssid not defined"
#endif


#ifndef WIFI_PASSWORD
//#error "password not defined"
#endif


#define TEST_MODE 1
#define WINDOW_SIZE 16
#define SAMPLING_FREQUENCY 50
#define MQTT_BUFFER_SIZE 512


const uint32_t sampling_interval = 1000 / SAMPLING_FREQUENCY;
const double dt = sampling_interval / 1000.0;
TaskHandle_t gatherTaskHandle;
TaskHandle_t filterTaskHandle;
TaskHandle_t MqttTaskHandle;
TaskHandle_t testMLTaskHandle;


QueueHandle_t filterQueue;


WiFiClient espClient;
PubSubClient mqttClient(espClient);

HallSensor* hallSensor = nullptr;

// Counter-based filename for SD card
char currentDataFile[32] = "/data_0.csv";

const char *mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;


QueueHandle_t mqttQueue;
Adafruit_INA219 ina;

// ── UUIDs ────────────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHAR_TX_UUID        "12345678-1234-1234-1234-123456789ab0"  // Notify → phone
#define CHAR_RX_UUID        "12345678-1234-1234-1234-123456789ab1"  // Write  ← phone

// ── Globals ──────────────────────────────────────────────────────────────────
static BLECharacteristic *pTxChar = nullptr;
static bool               deviceConnected = false;
static volatile bool      newDataAvailable = false;
static String             receivedData = "";
static SemaphoreHandle_t  dataMutex;

// ── BLE Callbacks ─────────────────────────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        deviceConnected = true;
        Serial.println("[BLE] Client connected");
    }
    void onDisconnect(BLEServer *pServer) override {
        deviceConnected = false;
        Serial.println("[BLE] Client disconnected — restarting advertising");
        pServer->startAdvertising();
    }
};

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        String value = pChar->getValue().c_str();
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
            receivedData = value;
            newDataAvailable = true;
            xSemaphoreGive(dataMutex);
        }
        Serial.printf("[BLE] Received: %s\n", value.c_str());
    }
};

// ── BLE Init ─────────────────────────────────────────────────────────────────
static void initBLE() {
    BLEDevice::init("Heltec-V3");

    BLEServer  *pServer  = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    // TX characteristic — notify
    pTxChar = pService->createCharacteristic(
        CHAR_TX_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxChar->addDescriptor(new BLE2902());

    // RX characteristic — write
    BLECharacteristic *pRxChar = pService->createCharacteristic(
        CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxChar->setCallbacks(new RxCallbacks());

    pService->start();

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.println("[BLE] Advertising started");
}
//SPIClass spi = SPIClass(SPI);
void wifi_connect()
{
#if !TEST_MODE
    //WiFi.begin(WIFI_SSID, WIFI_PASSWORD);


    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }


    Serial.println("\nWiFi connected");
#else
    Serial.println("TEST_MODE enabled: WiFi disabled");
#endif
}


void mqtt_reconnect()
{
#if !TEST_MODE
    while (!mqttClient.connected())
    {
        Serial.print("Connecting to MQTT...");


        if (mqttClient.connect("esp32_client"))
        {
            Serial.println("connected");
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" retrying...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
#endif
}



void mqtt_task(void *pvParameters)
{
#if !TEST_MODE
    mqttClient.setServer(mqtt_server, mqtt_port);


    const char *topic = "tzn/data";


    mpu_data_t data;
    int sample_count = 0;


    char payload[MQTT_BUFFER_SIZE];
    int offset = 0;


    for (;;)
    {
        if (!mqttClient.connected())
        {
            mqtt_reconnect();
        }


        mqttClient.loop();


        if (xQueueReceive(mqttQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            /* Append one sample to CSV */
            unsigned long timestamp_ms = millis();
            int written = snprintf(
                payload + offset,
                MQTT_BUFFER_SIZE - offset,
                "%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                timestamp_ms,
                data.ax, data.ay, data.az,
                data.gx, data.gy, data.gz
            );
          //Serial.printf("updating payload %s\n", payload);
            /* Check for overflow */
            if (written <= 0 || written >= (MQTT_BUFFER_SIZE - offset))
            {
                Serial.println("overflow, reset");
                // Buffer full → reset safely
                offset = 0;
                sample_count = 0;
                continue;
            }


            offset += written;
            sample_count++;


            /* When batch is ready */
            if (sample_count >= 6)
            {
                /* Remove last '\n' (optional but cleaner) */
                if (offset > 0)
                {
                    payload[offset - 1] = '\0';
                }


                mqttClient.publish(topic, payload);


                /* Reset buffer */
                offset = 0;
                sample_count = 0;
                payload[0] = '\0';
            }
        }
    }
#endif
}


void gatherTask(void* param){
    int i = 0;
  mpu_data_t data = {0};

  char buf[512];
  for(;;){
    readAccelGyro(&data);
    double velocity = hallSensor ? hallSensor->getSpeed() : 0.0;
    snprintf(buf, sizeof(buf), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f\n",
                 data.ax, data.ay, data.az, data.temp, data.gx, data.gy, data.gz, ina.getBusVoltage_V(),ina.getCurrent_mA(), velocity);
    //Serial.printf(buf);
    sd::write_csv(currentDataFile, buf);
    //xQueueSend(filterQueue, &data, portMAX_DELAY);
    //xQueueSend(mlQueue, &(data.az), portMAX_DELAY);
    //xQueueSend(mqttQueue, &(data),  portMAX_DELAY);
    if( i  == 0){
    snprintf(buf, sizeof(buf), "volt:%6.2f,vel:%6.2f\nax:%6.2f",ina.getBusVoltage_V(), velocity, data.az);
    display_message(buf);
  }
  i= (i+1) % 10;
    vTaskDelay(pdMS_TO_TICKS(sampling_interval));
  }

}


void filterTask(void* param){
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
  for(;;){
    xQueueReceive(filterQueue, &data, portMAX_DELAY);


    if (window_count == WINDOW_SIZE) {
      subSample(&running_sum, &window[index]);
    } else {
      window_count++;
    }


    window[index] = data;
    addSample(&running_sum, &data);
    index = (index + 1) % WINDOW_SIZE;


    divSample(&data_mean, &running_sum, (float)window_count);


    //Serial.printf(">mean_ax: %6.2f,mean_ay: %6.2f,mean_az: %6.2f, mean_temp:%6.2f, mean_gx: %6.2f, mean_gy: %6.2f, mean_gz:%6.2f\r\n",
    //              data_mean.ax, data_mean.ay, data_mean.az,
    //              data_mean.temp, data_mean.gx, data_mean.gy, data_mean.gz);
    roll_x += ((double)data.gx) * dt;
    pitch_y += ((double) data.gy) *dt;
    yaw_z += ((double)data.gz) * dt;
    roll_x_mean += ((double)data_mean.gx) * dt;
    pitch_y_mean += ((double) data_mean.gy) * dt;
    yaw_z_mean += ((double)data_mean.gz) * dt;


    double gx = 9.81 * sin(pitch_y_mean);
    double gy = 9.81 * cos(pitch_y_mean) * sin(roll_x_mean); 
    double gz = 9.81 * cos(pitch_y_mean) * cos(roll_x_mean);
    double decay = 0.995;//to limit drifting (clearly alters data so //TODO sensor fusion)
    vel_z += ((double) data.az - gz) * dt;
    vel_z_mean = vel_z_mean *decay +((double) data_mean.az - gz) * dt;
    pos_z += ((double) data.az - gz) * dt*dt*0.5 + vel_z * dt;
    pos_z_mean += ((double) data_mean.az - gz) * dt*dt*0.5 + vel_z_mean * dt;
    //Serial.printf(">roll_x:%f,roll_x_mean:%f,pitch_y:%f,pitch_y_mean:%f,yaw_z:%f,yaw_z_mean:%f,vel_z:%f,vel_z_mean:%f,pos_z:%f,pos_z_mean:%f\r\n",
    //              roll_x, roll_x_mean,
    //              pitch_y, pitch_y_mean,
    //              yaw_z, yaw_z_mean,
    //              vel_z, vel_z_mean,
    //              pos_z, pos_z_mean);
    // Serial.printf(">az:%f, gz_est:%f, diff:%f\r\n",data_mean.az, gz, data_mean.az - gz);
    
    // Not needed anymore, directly sending raw data to ML task
    //float z_to_send = data.az; 
    //xQueueSend(mlQueue, &z_to_send, 0); 



  }


}


void testMLTask(void* param) {
    mpu_data_t sample = {0};
    TickType_t lastWake = xTaskGetTickCount();

    for(;;) {
        Serial.println("--- OFF ROAD ---");

        for(int i = 0; i < 200; i++) {
            sample.ax = 0.15f + 0.04f * sinf(i * 0.20f);
            sample.ay = 0.40f + 0.06f * cosf(i * 0.18f);
            sample.az = 9.70f + 0.08f * sinf(i * 0.22f);
            sample.gx = 16.60f + 0.05f * sinf(i * 0.10f);
            sample.gy = 0.01f * sinf(i * 0.15f);
            sample.gz = 0.01f * cosf(i * 0.16f);
            sample.temp = 0.0f;

            xQueueSend(mlQueue, &sample, portMAX_DELAY);
            vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
        }

        Serial.println("--- NORMAL ROAD ---");

        for(int i = 0; i < 200; i++) {
            sample.ax = -0.8f + 1.2f * sinf(i * 0.23f) + 0.9f * cosf(i * 0.41f);
            sample.ay = 2.8f + 1.0f * sinf(i * 0.19f) + 1.1f * cosf(i * 0.33f);
            sample.az = 9.3f + 0.9f * sinf(i * 0.27f) + 1.0f * cosf(i * 0.37f);
            sample.gx = 18.8f + 0.35f * sinf(i * 0.11f) + 0.20f * cosf(i * 0.29f);
            sample.gy = 0.03f + 0.10f * sinf(i * 0.21f) + 0.06f * cosf(i * 0.31f);
            sample.gz = 0.02f + 0.22f * sinf(i * 0.17f) - 0.18f * cosf(i * 0.26f);
            sample.temp = 0.0f;

            if ((i % 37) == 0) {
                sample.ax -= 2.5f;
                sample.ay += 1.8f;
                sample.az += 2.8f;
                sample.gz -= 0.35f;
            }

            if ((i % 53) == 0) {
                sample.ax += 3.0f;
                sample.az -= 1.8f;
                sample.gy += 0.18f;
            }

            xQueueSend(mlQueue, &sample, portMAX_DELAY);
            vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
        }
    }
}

// Task 1 — handles incoming BLE data
void taskBLERx(void *pvParameters) {
    for (;;) {
        if (newDataAvailable) {
            String data;
            if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
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
void taskBLETx(void *pvParameters) {
    uint32_t counter = 0;
    for (;;) {
        if (deviceConnected && pTxChar) {
            String msg = "Hello from Heltec #" + String(counter++);
            pTxChar->setValue(msg.c_str());
            pTxChar->notify();
            Serial.printf("[TX Task] Sent: %s\n", msg.c_str());
        }
        vTaskDelay(pdMS_TO_TICKS(2000));  // send every 2 s
    }
}

void setup() {
    Heltec.begin(true, false, true);
  Serial.begin(115200);
  while (!Serial) delay(10);
  // FIX: Wire1.begin() MUST come before mpu_setup()
    display_init();
   // display_message("connecting");
   // wifi_connect();
   // display_message("wifi connected");
    // TEMP REMOVAL FOR TESTING WITH testMLTask
    Wire1.begin(SDA_PIN, SCL_PIN);
    Wire1.setClock(100000);
    ina.begin(&Wire1);
    ina.setCalibration_32V_2A();
    mpu_setup();
    display_message("loading calibration");
    
    // Try to load calibration data from Preferences
    if (!loadCalibrationFromPreferences()) {
      // If load fails, perform calibration
      display_message("calibrating mpu");
      calibrateMPU();
    }

    sd::init();
    
    // Read counter from SD card and create new filename for this boot
    uint32_t fileCounter = sd::read_counter();
    snprintf(currentDataFile, sizeof(currentDataFile), "/data_%lu.csv", fileCounter);
    Serial.printf("Current session data file: %s\n", currentDataFile);
    
    // Increment counter for next boot
    sd::increment_counter();
    sd::write_csv(currentDataFile, "ax,ay,az,gx,gy,gz,temp,volt,curr,vel\n");

    // Initialize hall sensor with 1 magnet and 15cm distance
    hallSensor = new HallSensor(33, 221, 1);  // GPIO 33, 150mm (15cm), 1 magnet


  //wifi_connect();
  //TEMP REMOVAL FOR TESTING WITH testMLTask
#if !TEST_MODE
  Wire1.begin(SDA_PIN, SCL_PIN);
  Wire1.setClock(100000);
  mpu_setup();


  calibrateMPU();
#endif
  
  filterQueue = xQueueCreate(WINDOW_SIZE * 2, sizeof(mpu_data_t));
  mqttQueue = xQueueCreate(10,sizeof(mpu_data_t));
  mlQueue = xQueueCreate(64, sizeof(mpu_data_t));



  setupML();

#if TEST_MODE
  xTaskCreatePinnedToCore(testMLTask, "testMLTask", 4096, NULL, 1, &testMLTaskHandle, 1);
#else
  //xTaskCreatePinnedToCore(gatherTask, "gatherTask", 4096, NULL, 1, &gatherTaskHandle, 1);
  xTaskCreatePinnedToCore(gatherTask, "gatherTask", 4096, NULL, 1, &gatherTaskHandle, 1);
  //xTaskCreatePinnedToCore(mqtt_task,"mqttTask",4096, NULL, 1,  &MqttTaskHandle, 0);
  xTaskCreatePinnedToCore(mqtt_task,"mqttTask",4096, NULL, 1,  &MqttTaskHandle, 0);
  //xTaskCreatePinnedToCore(filterTask, "filterTask", 4096, NULL, 1, &filterTaskHandle, 1);
#endif


    filterQueue = xQueueCreate(WINDOW_SIZE * 2, sizeof(mpu_data_t));
    mqttQueue = xQueueCreate(10, sizeof(mpu_data_t));
    mlQueue = xQueueCreate(10, sizeof(float));
    dataMutex = xSemaphoreCreateMutex();
    configASSERT(dataMutex);

    initBLE();

    // Pin tasks to specific cores (optional but good practice)
    xTaskCreatePinnedToCore(taskBLERx, "BLE_RX", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskBLETx, "BLE_TX", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(gatherTask, "gatherTask", 4096, NULL, 1, &gatherTaskHandle, 1);
    //xTaskCreatePinnedToCore(mqtt_task, "mqttTask", 4096, NULL, 1, &MqttTaskHandle, 0);
    // xTaskCreatePinnedToCore(filterTask, "filterTask", 4096, NULL, 1, &filterTaskHandle, 1);

    //setupML();

  }




void loop() {
  while(1){
    delay(1000);
  }
}