#include <Arduino.h>
#include <Wire.h>
#include "board.h"

#define WINDOW_SIZE 16
#define SAMPLING_FREQUENCY 100

const uint32_t sampling_interval = 1000 / SAMPLING_FREQUENCY;
const double dt = sampling_interval / 1000.0;
typedef struct mpu_data_t{
  float ax;
  float ay;
  float az;
  float temp;
  float gx;
  float gy;
  float gz;

}mpu_data_t;

TaskHandle_t gatherTaskHandle;
TaskHandle_t filterTaskHandle;

QueueHandle_t filterQueue;

float accelScale = 16384.0;  // updated after reading register
float gyroScale  = 131.0;    // updated after reading register
mpu_data_t mpuOffset = {0};

static inline void addSample(mpu_data_t* acc, const mpu_data_t* sample) {
  acc->ax += sample->ax;
  acc->ay += sample->ay;
  acc->az += sample->az;
  acc->temp += sample->temp;
  acc->gx += sample->gx;
  acc->gy += sample->gy;
  acc->gz += sample->gz;
}

static inline void subSample(mpu_data_t* acc, const mpu_data_t* sample) {
  acc->ax -= sample->ax;
  acc->ay -= sample->ay;
  acc->az -= sample->az;
  acc->temp -= sample->temp;
  acc->gx -= sample->gx;
  acc->gy -= sample->gy;
  acc->gz -= sample->gz;
}

static inline void divSample(mpu_data_t* out, const mpu_data_t* in, float divisor) {
  out->ax = in->ax / divisor;
  out->ay = in->ay / divisor;
  out->az = in->az / divisor;
  out->temp = in->temp / divisor;
  out->gx = in->gx / divisor;
  out->gy = in->gy / divisor;
  out->gz = in->gz / divisor;
}

static inline void applyCalibration(mpu_data_t* data, const mpu_data_t* offset) {
  data->ax -= offset->ax;
  data->ay -= offset->ay;
  data->az -= offset->az;
  data->gx -= offset->gx;
  data->gy -= offset->gy;
  data->gz -= offset->gz;
}

void writeReg(uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(MPU_ADDR);
  Wire1.write(reg);
  Wire1.write(val);
  Wire1.endTransmission(true);
}

uint8_t readReg(uint8_t reg) {
  Wire1.beginTransmission(MPU_ADDR);
  Wire1.write(reg);
  Wire1.endTransmission(false);
  Wire1.requestFrom(MPU_ADDR, 1);
  return Wire1.available() ? Wire1.read() : 0xFF;  // FIX: was Wire.read()
}

void mpu_setup() {
  // WHO_AM_I check
  Wire1.beginTransmission(MPU_ADDR);
  Wire1.write(0x75);
  Wire1.endTransmission(false);
  Wire1.requestFrom(MPU_ADDR, 1);
  if (Wire1.available()) {
    byte whoami = Wire1.read();
    Serial.printf("WHO_AM_I = 0x%02X\n", whoami);
  } else {
    Serial.println("No response from WHO_AM_I register");
  }

  // Wake up (clears sleep bit)
  writeReg(0x6B, 0x00);
  delay(100);
  Serial.println("Wake-up command sent");

  // Verify wake-up
  byte pwr = readReg(0x6B);
  Serial.printf("PWR_MGMT_1 = 0x%02X (expected 0x00)\n", pwr);

  // Write config
  writeReg(0x1A, 0x04);  // DLPF 21Hz
  writeReg(0x1B, 0x08);  // Gyro ±500°/s
  writeReg(0x1C, 0x10);  // FIX: 0x10 = ±8g, 0x00 = ±2g
  delay(50);

  // Read back and set scale factors
  uint8_t accelCfg = (readReg(0x1C) >> 3) & 0x03;
  uint8_t gyroCfg  = (readReg(0x1B) >> 3) & 0x03;

  Serial.printf("ACCEL_CONFIG bits: %d\n", accelCfg);
  Serial.printf("GYRO_CONFIG  bits: %d\n", gyroCfg);

  switch (accelCfg) {
    case 0: accelScale = 16384.0; Serial.println("Accel: ±2g");  break;
    case 1: accelScale = 8192.0;  Serial.println("Accel: ±4g");  break;
    case 2: accelScale = 4096.0;  Serial.println("Accel: ±8g");  break;
    case 3: accelScale = 2048.0;  Serial.println("Accel: ±16g"); break;
  }

  switch (gyroCfg) {
    case 0: gyroScale = 131.0; Serial.println("Gyro: ±250°/s");  break;
    case 1: gyroScale = 65.5;  Serial.println("Gyro: ±500°/s");  break;
    case 2: gyroScale = 32.8;  Serial.println("Gyro: ±1000°/s"); break;
    case 3: gyroScale = 16.4;  Serial.println("Gyro: ±2000°/s"); break;
  }
}

void readAccelGyro(mpu_data_t* data) {
  Wire1.beginTransmission(MPU_ADDR);
  Wire1.write(0x3B);
  Wire1.endTransmission(false);
  Wire1.requestFrom(MPU_ADDR, 14);

  uint8_t buf[14];
  for(int i = 0; i < 14; i++){
    buf[i] = Wire1.read();
  }
  // FIX: all Wire.read() changed to Wire1.read()
  data->ax  = ((int16_t) (buf[0]  << 8 | buf[1]))  / accelScale * 9.81;
  data->ay  = ((int16_t) (buf[2]  << 8 | buf[3]))  / accelScale * 9.81;
  data->az  = ((int16_t) (buf[4]  << 8 | buf[5]))  / accelScale * 9.81;
  data->temp =((int16_t) (buf[6]  << 8 | buf[7]))  /340.0f + 36.53f;
  data->gx  = ((int16_t) (buf[8]  << 8 | buf[9]))  / gyroScale * DEG_TO_RAD;
  data->gy  = ((int16_t) (buf[10] << 8 | buf[11])) / gyroScale * DEG_TO_RAD;
  data->gz  = ((int16_t) (buf[12] << 8 | buf[13])) / gyroScale * DEG_TO_RAD;

  applyCalibration(data, &mpuOffset);
}

void calibrateMPU() {
  const int calibrationSamples = 200;
  mpu_data_t sample = {0};
  mpu_data_t sum = {0};

  mpuOffset = {0};
  Serial.println("Starting MPU calibration (keep bike still)...");

  for (int i = 0; i < calibrationSamples; i++) {
    readAccelGyro(&sample);
    addSample(&sum, &sample);
    delay(5);
  }

  divSample(&mpuOffset, &sum, (float)calibrationSamples);
  mpuOffset.temp = 0.0f;

  Serial.printf("Calibration offsets -> ax:%6.3f ay:%6.3f az:%6.3f gx:%6.3f gy:%6.3f gz:%6.3f\n",
                mpuOffset.ax, mpuOffset.ay, mpuOffset.az,
                mpuOffset.gx, mpuOffset.gy, mpuOffset.gz);
}

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
  mpu_data_t running_sum = {0};
  mpu_data_t data = {0};
  mpu_data_t data_mean = {0};
  int window_count = 0;
  int index = 0;
  double theta_y = 0.0f;
  double omega_y = 0.0f;
  double theta_y_mean = 0.0f;
  double omega_y_mean = 0.0f;
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
    theta_y += ((double) data.gy) *dt;
    theta_y_mean += ((double) data_mean.gy) * dt;
    vel_z += ((double) data.az) * dt;
    vel_z_mean += ((double) data_mean.az) * dt;
    pos_z += ((double) data.az) * dt*dt*0.5 + vel_z * dt;
    pos_z_mean += ((double) data_mean.az) * dt*dt*0.5 + vel_z_mean * dt;
    Serial.printf(">theta_y:%f,theta_y_mean:%f,vel_z:%f,vel_z_mean:%f,pos_z:%f,pos_z_mean:%f\r\n",
            theta_y,theta_y_mean,vel_z,vel_z_mean,pos_z,pos_z_mean);

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