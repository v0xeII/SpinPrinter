// Stepper driver with a 10 MHz hardware timer.
// Every STEP edge is scheduled by the timer interrupt with a fractional (16.16) period
// accumulator, so the average speed is exact (no rounding to whole Hz) and every step is
// counted: the Raspberry Pi can ask for the precise vial angle and keep the video in sync.
#ifdef ARDUINO
#include "motor.h"

#include <Arduino.h>

#include "config.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"

static const uint32_t TICK_HZ = 10000000;  // timer resolution: 0.1 µs
static gptimer_handle_t gpt;
static esp_timer_handle_t rampTimer;

static volatile uint64_t halfFP = 0;  // half step period in ticks, 16.16 fixed point; 0 = idle
static volatile int32_t steps = 0;    // signed step counter (direction applied)
static volatile int dirSign = 1;
static uint32_t fracAcc = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static bool level = false;
static volatile bool moving = false, hitTarget = false;  // motor_move(): stop exactly on a step count
static volatile int32_t moveTarget = 0;

static MotorConfig C;
static volatile float target = 0, current = 0;
static volatile bool wantOn = false;
static bool dirState = false;  // current DIR pin level

// driver enable (LOW = on); PIN_EN -1: no pin, the driver's EN is tied to GND
static inline void enPin(int lvl) {
#if PIN_EN >= 0
  digitalWrite(PIN_EN, lvl);
#else
  (void)lvl;
#endif
}

static inline void stepPin(bool on) {
  if (on) REG_WRITE(GPIO_OUT_W1TS_REG, 1u << PIN_STEP);
  else REG_WRITE(GPIO_OUT_W1TC_REG, 1u << PIN_STEP);
}

static bool IRAM_ATTR onAlarm(gptimer_handle_t t, const gptimer_alarm_event_data_t* e, void*) {
  uint64_t h = halfFP;
  uint64_t next;
  if (h == 0) {
    if (level) { stepPin(false); level = false; }
    next = e->alarm_value + 2000;  // idle: look again in 200 µs
  } else {
    level = !level;
    stepPin(level);
    if (level) {
      portENTER_CRITICAL_ISR(&mux);
      steps += dirSign;
      bool hit = moving && steps == moveTarget;
      portEXIT_CRITICAL_ISR(&mux);
      if (hit) {  // integer-only work here: no floating point inside an interrupt
        halfFP = 0;
        moving = false;
        hitTarget = true;
      }
    }
    uint64_t sum = (uint64_t)fracAcc + (h & 0xFFFF);
    fracAcc = sum & 0xFFFF;
    next = e->alarm_value + (h >> 16) + (sum >> 16);
  }
  gptimer_alarm_config_t a = {};
  a.alarm_count = next;
  gptimer_set_alarm_action(t, &a);
  return false;
}

static float microPerVialRev() { return C.stepsPerRev * C.microsteps * C.gear; }

// 5 ms: ramp the speed, handle direction changes and the driver enable pin
static void ramp(void*) {
  if (hitTarget) {
    hitTarget = false;
    wantOn = false;
    current = 0;
  }
  bool wantDir = C.dirInvert;
  float tgt = wantOn ? target : 0;
  if (moving) {  // brake in time to land on the target step
    float remaining = (float)abs(moveTarget - steps);
    float aSteps = C.accel / 60.f * microPerVialRev();
    float vMax = sqrtf(2.f * aSteps * remaining) / microPerVialRev() * 60.f;
    if (vMax < 0.3f) vMax = 0.3f;
    if (tgt > vMax) tgt = vMax;
  }
  if (wantDir != dirState) tgt = 0;  // slow down, flip, speed up again
  float a = C.accel * 0.005f;
  float d = tgt - current;
  float cur = current + (d > a ? a : (d < -a ? -a : d));
  if (cur < 0.0005f) cur = 0;
  current = cur;
  if (cur == 0 && wantDir != dirState) {
    dirState = wantDir;
    digitalWrite(PIN_DIR, dirState ? HIGH : LOW);
    dirSign = dirState ? -1 : 1;
  }
  float stepHz = cur / 60.f * microPerVialRev();
  if (stepHz < 1.f) {
    halfFP = 0;
    if (!wantOn && cur == 0) enPin(HIGH);  // driver off: cool and silent
    return;
  }
  enPin(LOW);
  double half = (double)TICK_HZ / (2.0 * stepHz);
  if (half < 20) half = 20;  // 250 kHz step limit
  halfFP = (uint64_t)(half * 65536.0);
}

void motor_init(const MotorConfig& cfg) {
  C = cfg;
#if PIN_EN >= 0
  pinMode(PIN_EN, OUTPUT);
#endif
  enPin(HIGH);
  pinMode(PIN_DIR, OUTPUT);
  dirState = C.dirInvert;
  dirSign = dirState ? -1 : 1;
  digitalWrite(PIN_DIR, dirState ? HIGH : LOW);
  pinMode(PIN_STEP, OUTPUT);
  digitalWrite(PIN_STEP, LOW);

  gptimer_config_t tc = {};
  tc.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  tc.direction = GPTIMER_COUNT_UP;
  tc.resolution_hz = TICK_HZ;
  gptimer_new_timer(&tc, &gpt);
  gptimer_event_callbacks_t cb = {};
  cb.on_alarm = onAlarm;
  gptimer_register_event_callbacks(gpt, &cb, nullptr);
  gptimer_alarm_config_t a = {};
  a.alarm_count = 2000;
  gptimer_set_alarm_action(gpt, &a);
  gptimer_enable(gpt);
  gptimer_start(gpt);

  esp_timer_create_args_t args = {};
  args.callback = ramp;
  args.name = "ramp";
  esp_timer_create(&args, &rampTimer);
  esp_timer_start_periodic(rampTimer, 5000);
}

void motor_config(const MotorConfig& cfg) {
  // keep the angle continuous when the step resolution changes
  double ang = motor_angle();
  portENTER_CRITICAL(&mux);
  C = cfg;
  steps = (int32_t)(ang / 360.0 * microPerVialRev());
  portEXIT_CRITICAL(&mux);
}

void motor_set(bool on, float rpm) {
  moving = false;
  wantOn = on;
  target = rpm;
}

void motor_move(double deg, float rpm) {
  int32_t n = (int32_t)llround(fabs(deg) / 360.0 * microPerVialRev());
  portENTER_CRITICAL(&mux);
  moveTarget = steps + (C.dirInvert ? -1 : 1) * n;  // the direction the motor will turn
  portEXIT_CRITICAL(&mux);
  moving = true;
  hitTarget = false;
  target = rpm;
  wantOn = true;
}
bool motor_moving() { return moving; }
float motor_rpm() { return current; }
bool motor_at_speed() { return wantOn && !moving && fabsf(current - target) < 1e-4f; }
double motor_angle() { return (double)steps * 360.0 / microPerVialRev(); }
void motor_set_angle(double deg) {
  portENTER_CRITICAL(&mux);
  steps = (int32_t)llround(deg / 360.0 * microPerVialRev());
  portEXIT_CRITICAL(&mux);
}

void motor_zero() {
  portENTER_CRITICAL(&mux);
  steps = 0;
  portEXIT_CRITICAL(&mux);
}
#endif
