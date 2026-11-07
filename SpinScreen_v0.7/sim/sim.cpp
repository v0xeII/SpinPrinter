// PC simulator: runs the real UI code with a scripted user + fake Pi, writes RGB24 frames to stdout.
// build: gcc -O2 -c ../SpinScreen/qrcodegen.c -o qr.o && g++ -O2 -std=c++17 -I../SpinScreen sim.cpp ../SpinScreen/ui.cpp qr.o -o sim
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "config.h"
#include "gfx.h"
#include "ui.h"

static float curRpm = 0, tgtRpm = 0, accel = 6, moveLeft = -1;
static double angle = 0;
static int dirSign = 1;
struct Ev { int ms; Key k; const char* line; };
static std::vector<Ev> ev;
static int nowMs = 0;
static bool lit = false;  // the projector light (for the demo animation): on from the first PROG to DONE
static void line(int ms, const char* l) { ev.push_back({ms, K_NONE, strdup(l)}); }
static void key(int ms, Key k) { ev.push_back({ms, k, nullptr}); }
static void hSend(const char* l) {
  fprintf(stderr, "ESP> %s\n", l);
  if (!strcmp(l, "EV LIST")) {  // pretend to be the Pi
    int t = nowMs + 350;
    line(t, "FL BEGIN 3 0 3 5");
    line(t, "FL 0 4.00 1 6.40 4.0 15.000 1 benchy_spinslicer.mp4");
    line(t, "FL 1 4.00 0 5.35 6.0 15.000 1 lattice_cube_1280x800.mp4");
    line(t, "FL 2 9.00 1 -1 -1 6.667 0 opencal_thinker_9rpm.mp4");
    line(t, "FL END");
  }
  if (!strncmp(l, "EV PRINT", 8)) {
    int t = nowMs + 300;
    line(t, "FILE lattice_cube_1280x800.mp4");
    line(t, "RPM 10");
    line(t, "SPIN 1");
    line(t, "STATE PRINT");
    line(t, "TURNS 6");
    line(t + 100, "WAIT 2 spin");
    line(t + 1100, "WAIT 1 spin");
    line(t + 1600, "WAIT 3 dark");
    line(t + 2600, "WAIT 2 dark");
    line(t + 3600, "WAIT 1 dark");
    t += 4400;
    for (int i = 0; i <= 30; i++) {
      char b[40];
      snprintf(b, sizeof b, "PROG %d %d", i * 100 / 30, 90 - i * 3);
      line(t + i * 130, b);
      snprintf(b, sizeof b, "SYNC %d", 2 + (i * 7) % 4);
      line(t + 50 + i * 130, b);
    }
    line(t + 31 * 130, "STATE DONE");
    line(t + 31 * 130, "SPIN 0");
  }
}
static void hSpin(bool o, float r) { tgtRpm = o ? r : 0; moveLeft = -1; }
static void hBright(int) {}
static void hSave(const Settings&) {}
static float hRpm() { return curRpm; }
static double hAngle() { return angle; }
static void hCfg(const Settings& s) { accel = s.accel; dirSign = s.dirInv ? -1 : 1; }
static void hZero() { angle = 0; }
static void hMove(double deg, float rpm) { moveLeft = (float)deg; tgtRpm = rpm; }

int main(int argc, char** argv) {
  int W = 320, H = 170;
  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "--size=", 7)) sscanf(argv[i] + 7, "%dx%d", &W, &H);
  std::vector<uint16_t> fb(W * H);
  Canvas c(W, H, fb.data());
  UiHal hal{hSend, hSpin, hBright, hSave, hRpm, hAngle, hCfg, hZero, hMove};
  Settings s{4.0f, 0, 80, 16, 200, 6.f, 0, 1.0f, 1, 1, 0};
  FILE* track = nullptr;  // --track=FILE: "ms angle lit" per frame, for dev/make_demo.py
  int stopMs = 0;         // --until=MS: stop early
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "en")) s.lang = 1;
    if (!strcmp(argv[i], "card")) s.home = HOME_CARD;   // the firmware default: the big PRINT card instead of Spinny
    if (!strncmp(argv[i], "--track=", 8)) track = fopen(argv[i] + 8, "w");
    if (!strncmp(argv[i], "--until=", 8)) stopMs = atoi(argv[i] + 8);
  }
  ui_init(hal, s, 0);
  for (int ms = 1500; ms < 60000; ms += 1000) line(ms, "PING");
  line(1600, "URL http://192.168.1.42");
  line(1600, "HOST spinslicer.local");
  line(1600, "NET SpinSlicer-5G");
  line(1600, "CAL 0 400");
  int T = 3000;
  bool axoDemo = false;
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "axo")) axoDemo = true;
  bool idle = false;  // nobody touches the buttons: boot, then the main page
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "idle")) idle = true;
  bool behave = false;
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "behave")) behave = true;
  if (idle) { T = 8000; goto run; }
  if (behave) {  // Spinny on his own: scroll a bit, then leave him alone (food at ~30 s, sleep after 60 s)
    for (int i = 0; i < 6; i++) key(T + 400 + i * 500, i < 3 ? K_DOWN : K_UP);
    key(T + 4000, K_OK);  // a happy hop
    T = 95000;
    goto run;
  }
  if (axoDemo) {  // Сервис -> Кастомизация: every colour, then the next main-screen choice
    key(T, K_DOWN); key(T += 300, K_DOWN); key(T += 400, K_OK);
    for (int i = 0; i < 3; i++) key(T += 300, K_DOWN);
    key(T += 500, K_OK);                          // Аксолотль
    key(T += 800, K_DOWN);                        // Окраска
    for (int i = 0; i < 6; i++) key(T += 1700, K_OK);
    key(T += 900, K_UP); key(T += 500, K_OK);     // Показывать: выкл
    key(T += 1500, K_BACK);
    key(T += 1500, K_BACK);
    key(T += 1500, K_DOWN);
    T += 1500;
    goto run;
  }
  key(T, K_OK);                                   // Печать -> list
  key(T += 1400, K_DOWN); key(T += 500, K_OK);    // second video -> setup
  key(T += 1200, K_UP); key(T += 350, K_UP);      // to the correction
  key(T += 500, K_OK);                            // edit correction
  for (int i = 0; i < 10; i++) key(T += 260, K_UP);
  key(T += 700, K_OK);
  key(T += 500, K_DOWN); key(T += 350, K_DOWN);   // back to the button
  key(T += 900, K_OK);                            // PRINT
  T += 9300;
  key(T += 600, K_UP);                            // feedback: under-cured
  key(T += 900, K_OK);
  key(T += 900, K_DOWN); key(T += 300, K_DOWN); key(T += 400, K_OK);  // Сервис
  key(T += 900, K_OK);                            // Проектор
  for (int i = 0; i < 5; i++) key(T += 260, K_DOWN);
  key(T += 500, K_OK);                            // Центровка
  for (int i = 0; i < 6; i++) key(T += 200, K_UP);
  key(T += 700, K_OK);
  key(T += 500, K_BACK);
  key(T += 800, K_DOWN); key(T += 400, K_OK);     // Мотор
  for (int i = 0; i < 7; i++) key(T += 220, K_DOWN);
  key(T += 500, K_OK);                            // ровно 1 оборот
  T += 3400;
  key(T += 300, K_BACK);
  key(T += 700, K_DOWN); key(T += 300, K_DOWN); key(T += 400, K_OK);  // Raspberry Pi
  key(T += 700, K_DOWN); key(T += 400, K_OK);     // Выключить -> Точно?
  key(T += 1500, K_BACK);
run:
  int endMs = stopMs ? stopMs : T + 1200;

  std::vector<unsigned char> rgb(W * H * 3);
  const int fps = 60;
  for (int f = 0; f * 1000 / fps < endMs; f++) {
    int ms = f * 1000 / fps, prev = (f - 1) * 1000 / fps;
    nowMs = ms;
    for (size_t i = 0; i < ev.size(); i++) {
      Ev e = ev[i];
      if (e.ms > prev && e.ms <= ms) {
        if (e.line) {
          fprintf(stderr, "PI > %s\n", e.line);
          ui_line(e.line, ms);
          if (!strncmp(e.line, "PROG", 4)) lit = true;
          if (!strncmp(e.line, "STATE DONE", 10)) lit = false;
        }
        else ui_key(e.k, ms);
      }
    }
    float dt = 1.f / fps, a = accel * dt;
    curRpm += clampf(tgtRpm - curRpm, -a, a);
    angle += dirSign * curRpm * 6.0 * dt;
    if (moveLeft >= 0) {
      moveLeft -= curRpm * 6.0f * dt;
      float vmax = sqrtf(2 * accel * 6 * fmaxf(moveLeft, 0)) / 6;
      if (tgtRpm > vmax) tgtRpm = fmaxf(vmax, 0.3f);
      if (moveLeft <= 0) { curRpm = tgtRpm = 0; moveLeft = -1; }
    }
    (void)ui_frame(c, ms);
    if (track) fprintf(track, "%d %.3f %d\n", ms, angle, lit ? 1 : 0);
    for (int i = 0; i < W * H; i++) {
      uint16_t p = fb[i];
      rgb[i * 3] = (p >> 11) << 3 | (p >> 13);
      rgb[i * 3 + 1] = ((p >> 5) & 63) << 2 | ((p >> 9) & 3);
      rgb[i * 3 + 2] = (p & 31) << 3 | ((p >> 2) & 7);
    }
    fwrite(rgb.data(), 1, rgb.size(), stdout);
  }
  return 0;
}
