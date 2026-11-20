#pragma once
#include <stdint.h>

struct MotorConfig {
  int stepsPerRev;  // full steps of the motor (200 for a 1.8° NEMA17)
  int microsteps;   // as set on the driver
  float gear;       // vial turns = motor turns / gear
  float accel;      // rpm per second
  bool dirInvert;
};

void motor_init(const MotorConfig& cfg, int stepPin, int dirPin, int enPin);  // enPin -1: EN tied to GND
void motor_config(const MotorConfig& cfg);  // safe to call while running
void motor_set(bool on, float rpm);         // rpm of the vial
float motor_rpm();                          // current vial rpm (while ramping)
double motor_angle();                       // vial angle in degrees since last zero (grows without wrapping)
void motor_zero();                          // angle := 0 now
void motor_set_angle(double deg);           // angle := deg now (the Pi lines the vial up with the video)
bool motor_at_speed();                      // finished ramping to the target
void motor_move(double deg, float rpm);     // turn exactly this many degrees, then stop
bool motor_moving();
