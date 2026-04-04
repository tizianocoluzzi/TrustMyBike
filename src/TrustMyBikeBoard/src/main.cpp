#include <Arduino.h>
#include <Wire.h>
#include "board.h"
#include "sensors/mpu.h"
#include "config.h"
#include "filters/filters.h"

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
    xQueueSend(filterQueue, &data, portMAX_DELAY);
    delay(sampling_interval);
  }
}

void filterTask(void* param){
  mpu_data_t window[WINDOW_SIZE] = {0};
  mpu_data_t running_sum = {0.0f};
  mpu_data_t data = {0.0f};
  mpu_data_t data_mean = {0.0f};
  int window_count = 0;
  int index = 0;
  angles_t angle = {0.0f};
  angles_t angle_mean = {0.0f};
  velocity_t velocity = {0.0f};
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

   integrate_angles(&data, &angle, dt);
   integrate_angles(&data_mean, &angle_mean, dt);
   integrate_velocity(&data_mean, &angle_mean, &velocity,dt);

  Serial.printf(
    ">angle_roll: %f,angle_pitch: %f,angle_yaw: %f,angle_mean_roll: %f,angle_mean_pitch: %f,angle_mean_yaw: %f,data_ax: %f,data_ay: %f,data_az: %f,data_temp: %f,data_gx: %f,data_gy: %f,data_gz: %f,data_mean_ax: %f,data_mean_ay: %f,data_mean_az: %f,data_mean_temp: %f,data_mean_gx: %f,data_mean_gy: %f,data_mean_gz: %f,velocity_z: %f\r\n",
    angle.roll, angle.pitch, angle.yaw,
    angle_mean.roll, angle_mean.pitch, angle_mean.yaw,
    data.ax, data.ay, data.az, data.temp, data.gx, data.gy, data.gz,
    data_mean.ax, data_mean.ay, data_mean.az, data_mean.temp, data_mean.gx, data_mean.gy, data_mean.gz,
    velocity.vel_z
  );

  }

}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  // FIX: Wire1.begin() MUST come before mpu_setup()
  Wire1.begin(SDA_PIN, SCL_PIN);
  Wire1.setClock(100000);

  mpu_setup();
  calibrateMPU();
  filterQueue = xQueueCreate(WINDOW_SIZE * 2, sizeof(mpu_data_t));
  xTaskCreatePinnedToCore(gatherTask, "gatherTask", 4096, NULL, 1, &gatherTaskHandle, 1);
  xTaskCreatePinnedToCore(filterTask, "filterTask", 4096, NULL, 1, &filterTaskHandle, 1);
}



void loop() {
  while(1){
    delay(1000);
  }
}