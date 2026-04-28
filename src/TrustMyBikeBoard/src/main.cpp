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

#ifndef WIFI_SSID
#error "ssid not defined"
#endif

#ifndef WIFI_PASSWORD
#error "password not defined"
#endif

#define WINDOW_SIZE 16
#define SAMPLING_FREQUENCY 100
#define MQTT_BUFFER_SIZE 512

const uint32_t sampling_interval = 1000 / SAMPLING_FREQUENCY;
const double dt = sampling_interval / 1000.0;
TaskHandle_t gatherTaskHandle;
TaskHandle_t filterTaskHandle;
TaskHandle_t MqttTaskHandle;

QueueHandle_t filterQueue;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

HallSensor* hallSensor = nullptr;

const char *mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

QueueHandle_t mqttQueue;
Adafruit_INA219 ina;
//SPIClass spi = SPIClass(SPI);
void wifi_connect()
{
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nWiFi connected");
}

void mqtt_reconnect()
{
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
}


void mqtt_task(void *pvParameters)
{
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
}

void gatherTask(void* param){
    int i = 0;
  mpu_data_t data = {0};

  char buf[512];
  for(;;){
    readAccelGyro(&data);
    double velocity = hallSensor ? hallSensor->getSpeed() : 0.0;
    snprintf(buf, sizeof(buf), ">ax:%6.2f,ay:%6.2f,az:%6.2f,temp:%6.2f,gx:%6.2f,gy:%6.2f,gz:%6.2f,volt:%6.2f,vel:%6.2f\r\n",
                 data.ax, data.ay, data.az, data.temp, data.gx, data.gy, data.gz, ina.getBusVoltage_V(), velocity);
    //Serial.printf(buf);
    sd::write_csv("/data.csv", buf);
    //xQueueSend(filterQueue, &data, portMAX_DELAY);
    //xQueueSend(mlQueue, &(data.az), portMAX_DELAY);
    //xQueueSend(mqttQueue, &(data),  portMAX_DELAY);
    if( i % 100 == 0){
    snprintf(buf, sizeof(buf), "volt:%6.2f,vel:%6.2f",ina.getBusVoltage_V(), velocity);
    display_message(buf);
  }
  i= (i+1) % 100;
    delay(sampling_interval);
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
    
    float z_to_send = data.az; 
    xQueueSend(mlQueue, &z_to_send, 0); 

  }

}

void testMLTask(void* param) {
    float normal_z = 9.81; // 1G, smooth road
    float bump_z = 18.5;   // Huge spike from a pothole

    for(;;) {
        // Send normal road data for 2 seconds (100 samples at 20ms)
        for(int i = 0; i < 100; i++) {
            xQueueSend(mlQueue, &normal_z, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(20)); 
        }

        // Inject a massive bump!
        Serial.println("--- INJECTING FAKE BUMP ---");
        xQueueSend(mlQueue, &bump_z, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(20));
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
    mpu_setup();
    display_message("calibrating mpu");
    calibrateMPU();

    sd::init();

    // Initialize hall sensor with 1 magnet and 15cm distance
    hallSensor = new HallSensor(33, 150, 1);  // GPIO 33, 150mm (15cm), 1 magnet

    filterQueue = xQueueCreate(WINDOW_SIZE * 2, sizeof(mpu_data_t));
    mqttQueue = xQueueCreate(10, sizeof(mpu_data_t));
    mlQueue = xQueueCreate(10, sizeof(float));

    xTaskCreatePinnedToCore(gatherTask, "gatherTask", 4096, NULL, 1, &gatherTaskHandle, 1);
    //xTaskCreatePinnedToCore(mqtt_task, "mqttTask", 4096, NULL, 1, &MqttTaskHandle, 0);
    // xTaskCreatePinnedToCore(filterTask, "filterTask", 4096, NULL, 1, &filterTaskHandle, 1);

    setupML();

  }



void loop() {
  while(1){
    delay(1000);
  }
}