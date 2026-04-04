#pragma once
#include "config.h"
#include "sensors/mpu.h"

typedef struct {
    double roll;
    double pitch;
    double yaw;
}angles_t;

static inline void integrate_angles(mpu_data_t* data,angles_t* angles, double dt ){
    angles->roll += ((double) data->gx) * dt;
    angles->pitch +=((double) data->gy) * dt;
    angles->yaw +=  ((double) data->gz) * dt;
}

typedef struct{
    double vel_x;
    double vel_y;
    double vel_z;
}velocity_t;

static inline void integrate_velocity(mpu_data_t* data, angles_t* angles, velocity_t *velocity, double dt){

    double gx = 9.81 * sin(angles->pitch);
    double gy = 9.81 * cos(angles->pitch) * sin(angles->roll); 
    double gz = 9.81 * cos(angles->pitch) * cos(angles->roll);
    //double decay = 0.995;//to limit drifting (clearly alters data so //TODO sensor fusion)
    velocity->vel_z += ((double) data->az - gz) * dt;
}