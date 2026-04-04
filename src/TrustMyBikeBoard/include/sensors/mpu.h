#pragma once
#include <Arduino.h>
typedef struct mpu_data_t{
  float ax;
  float ay;
  float az;
  float temp;
  float gx;
  float gy;
  float gz;

}mpu_data_t;



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
static inline void applyCalibration(mpu_data_t* data, const mpu_data_t* offset);
void writeReg(uint8_t reg, uint8_t val);
uint8_t readReg(uint8_t reg);
void mpu_setup();
void readAccelGyro(mpu_data_t* data);
void calibrateMPU();