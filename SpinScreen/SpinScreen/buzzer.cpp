// The buzzer (see buzzer.h): notes are played by a small task, so the screen never waits for them.
#include "buzzer.h"

#include <Arduino.h>

struct Note { uint16_t hz, ms; };  // hz 0 = a rest
static const Note N_TICK[] = {{3800, 4}, {0, 0}};
static const Note N_DONE[] = {{1047, 80}, {0, 25}, {1319, 80}, {0, 25}, {1568, 170}, {0, 0}};  // C E G, up
static const Note N_ERR[] = {{660, 120}, {0, 70}, {523, 180}, {0, 0}};                         // two low, down
static const Note N_ASK[] = {{1568, 70}, {0, 30}, {2093, 90}, {0, 0}};                         // a question
static const Note N_TEST[] = {{1760, 120}, {0, 0}};
static const Note* const MELODY[] = {N_TICK, N_DONE, N_ERR, N_ASK, N_TEST};

// peak duty of 1023 (10 bits). A passive buzzer is loudest at 50 %; a few percent is a soft voice.
static const int PEAK[] = {0, 22, 140};
static const int CH = 2;  // LEDC channel 2 (its own timer): channel 0 is the backlight (display.cpp)

struct Req { int8_t kind, a, b; };  // kind 0: play sound a at level b · 1: buzzer on pin a
static QueueHandle_t q;
static int pinNow = -1;        // the task's pin
static int pinWanted = -1;     // the caller's idea of it (buzzer_play needs no lock for it)
static volatile bool busy = false;
static uint32_t lastMs = 0;

static void setPin(int pin) {
  if (pin == pinNow) return;
  if (pinNow >= 0) {
    ledcDetach(pinNow);
    pinMode(pinNow, INPUT);  // let go of the old pin
  }
  pinNow = -1;
  if (pin >= 0 && ledcAttachChannel(pin, 2000, 10, CH)) {
    ledcWrite(pin, 0);
    pinNow = pin;
  }
}

static void note(const Note& n, int peak) {
  if (!n.hz || pinNow < 0) {
    vTaskDelay(pdMS_TO_TICKS(n.ms));
    return;
  }
  ledcChangeFrequency(pinNow, n.hz, 10);
  // fade in 6 ms, out 12 ms: no click at the edges, and it sounds rounder
  int in = n.ms >= 40 ? 6 : 0, out = n.ms >= 40 ? 12 : 0;
  for (int t = 0; t < n.ms; t++) {
    float k = 1;
    if (t < in) k = (t + 1) / (float)in;
    else if (t >= n.ms - out) k = (n.ms - t) / (float)out;
    ledcWrite(pinNow, (uint32_t)(peak * k));
    vTaskDelay(1);
  }
  ledcWrite(pinNow, 0);
}

static void task(void*) {
  for (;;) {
    Req r;
    if (xQueueReceive(q, &r, portMAX_DELAY) != pdTRUE) continue;
    if (r.kind == 1) {
      setPin(r.a);
      continue;
    }
    busy = true;
    int peak = PEAK[r.b];
    if (r.a == SND_TICK) peak = peak * 2 / 3;
    for (const Note* n = MELODY[r.a]; n->ms; n++) note(*n, peak);
    busy = false;
  }
}

void buzzer_init() {
  q = xQueueCreate(4, sizeof(Req));
  xTaskCreatePinnedToCore(task, "buzz", 2048, nullptr, 2, nullptr, 0);
}

void buzzer_pin(int pin) {
  pinWanted = pin;
  Req r{1, (int8_t)pin, 0};
  if (q) xQueueSend(q, &r, pdMS_TO_TICKS(50));
}

void buzzer_play(int sound, int level) {
  if (!q || pinWanted < 0 || level <= SOUND_OFF || level > SOUND_LOUD || sound < 0 || sound > SND_TEST) return;
  uint32_t now = millis();
  if (sound == SND_TICK) {
    if (busy || uxQueueMessagesWaiting(q)) return;  // a click never queues up behind a chime
  } else {
    if (sound != SND_TEST && now - lastMs < 1500) return;  // one sound at a time, never a string of them
    lastMs = now;
  }
  Req r{0, (int8_t)sound, (int8_t)level};
  xQueueSend(q, &r, 0);
}
