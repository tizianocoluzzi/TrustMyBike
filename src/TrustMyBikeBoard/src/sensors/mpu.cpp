#include "Wire.h"
#include "board.h"
#include "sensors/mpu.h"

float accelScale = 16384.0;  // updated after reading register
float gyroScale  = 131.0;    // updated after reading register
mpu_data_t mpuOffset = {0};



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
  const int calibrationSamples = 1000;
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
  mpuOffset.az -= 9.81; //assuming acceleration on z-axis
  Serial.printf("Calibration offsets -> ax:%6.3f ay:%6.3f az:%6.3f gx:%6.3f gy:%6.3f gz:%6.3f\n",
                mpuOffset.ax, mpuOffset.ay, mpuOffset.az,
                mpuOffset.gx, mpuOffset.gy, mpuOffset.gz);
}