#include <Arduino.h>
#include <Wire.h>
#include "board.h"
#include "sensors/mpu.h"
#include "ml/inference.h"
#define WINDOW_SIZE 16
#define SAMPLING_FREQUENCY 100

const uint32_t sampling_interval = 1000 / SAMPLING_FREQUENCY;
const double dt = sampling_interval / 1000.0;
TaskHandle_t gatherTaskHandle;
TaskHandle_t filterTaskHandle;

QueueHandle_t filterQueue;



void gatherTask(void* param){
  mpu_data_t data = {0};
  for(;;){
    readAccelGyro(&data);
   // Serial.printf(">ax: %6.2f,ay: %6.2f,az: %6.2f, temp:%6.2f, gx: %6.2f, gy: %6.2f, gz:%6.2f\r\n",
   //               data.ax, data.ay, data.az, data.temp, data.gx, data.gy, data.gz);
    //xQueueSend(filterQueue, &data, portMAX_DELAY);
    xQueueSend(mlQueue, &(data.az), portMAX_DELAY);
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
    Serial.printf(">roll_x:%f,roll_x_mean:%f,pitch_y:%f,pitch_y_mean:%f,yaw_z:%f,yaw_z_mean:%f,vel_z:%f,vel_z_mean:%f,pos_z:%f,pos_z_mean:%f\r\n",
                  roll_x, roll_x_mean,
                  pitch_y, pitch_y_mean,
                  yaw_z, yaw_z_mean,
                  vel_z, vel_z_mean,
                  pos_z, pos_z_mean);
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
  Serial.begin(115200);
  while (!Serial) delay(10);
  
  // FIX: Wire1.begin() MUST come before mpu_setup()

  //TEMP REMOVAL FOR TESTING WITH testMLTask
  Wire1.begin(SDA_PIN, SCL_PIN);
  Wire1.setClock(100000);
  mpu_setup();
  calibrateMPU();
  filterQueue = xQueueCreate(WINDOW_SIZE * 2, sizeof(mpu_data_t));
  //TEMP REMOVAL FOR TESTING WITH testMLTask

  mlQueue = xQueueCreate(10, sizeof(float));
  //TEMP REMOVAL FOR TESTING WITH testMLTask

  xTaskCreatePinnedToCore(gatherTask, "gatherTask", 4096, NULL, 1, &gatherTaskHandle, 1);
  //xTaskCreatePinnedToCore(filterTask, "filterTask", 4096, NULL, 1, &filterTaskHandle, 1);

  setupML();
  //FAKE DATA INJECTOR
  //xTaskCreatePinnedToCore(testMLTask, "testTask", 4096, NULL, 1, NULL, 1);

  }



void loop() {
  while(1){
    delay(1000);
  }
}