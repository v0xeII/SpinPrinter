// Screenshots of every screen at one size, drawn by the real UI (ui.cpp) with a scripted user and
// a fake Pi. Writes one PPM per screen into a folder; dev/make_shots.py puts them on one sheet.
//   ./shots 320x170 out/          (also: round  = the round GC9A01 layout, axo = Spinny on the
//   ./shots 240x240 round out/     main page instead of the card, en = English)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "gfx.h"
#include "ui.h"

static int W = 320, H = 170, nowMs = 0, shotN = 0;
static std::string dir = ".";
static std::vector<uint16_t> fb;
static float curRpm = 0, tgtRpm = 0;
static double angle = 0;
static std::vector<std::string> pending;  // lines from the fake Pi, delivered on the next frame

static void hSend(const char* l) {
  if (!strcmp(l, "EV LIST")) {
    pending.push_back("FL BEGIN 3 0 3 5");
    pending.push_back("FL 0 4.00 1 6.40 4.0 15.000 1 benchy_spinslicer.mp4");
    pending.push_back("FL 1 4.00 0 5.35 6.0 15.000 1 lattice_cube_1280x800.mp4");
    pending.push_back("FL 2 9.00 1 -1 -1 6.667 0 opencal_thinker_9rpm.mp4");
    pending.push_back("FL END");
  }
}
static void hSpin(bool o, float r) { tgtRpm = o ? r : 0; }
static void hBright(int) {}
static void hSave(const Settings&) {}
static float hRpm() { return curRpm; }
static double hAngle() { return angle; }
static void hCfg(const Settings&) {}
static void hZero() { angle = 0; }
static void hMove(double, float rpm) { tgtRpm = rpm; }

static Canvas* cv;
static void frames(int ms) {  // run the UI at 60 fps for this long
  for (int end = nowMs + ms; nowMs < end; nowMs += 16) {
    for (auto& l : pending) ui_line(l.c_str(), nowMs);
    pending.clear();
    if (nowMs % 1000 < 16) ui_line("PING", nowMs);
    curRpm += (tgtRpm - curRpm) * 0.05f;
    angle += curRpm * 6.0 * 0.016;
    ui_frame(*cv, nowMs);
  }
}
static void key(Key k, int wait = 450) { ui_key(k, nowMs); frames(wait); }
static void line(const char* l) { ui_line(l, nowMs); }
static void shot(const char* name) {
  char fn[512];
  snprintf(fn, sizeof fn, "%s/%02d_%s.ppm", dir.c_str(), shotN++, name);
  FILE* f = fopen(fn, "wb");
  if (!f) { perror(fn); exit(1); }
  fprintf(f, "P6 %d %d 255\n", W, H);
  for (auto p : fb) {
    fputc((p >> 11) << 3 | (p >> 13), f);
    fputc(((p >> 5) & 63) << 2 | ((p >> 9) & 3), f);
    fputc((p & 31) << 3 | ((p >> 2) & 7), f);
  }
  fclose(f);
}

int main(int argc, char** argv) {
  bool round = false, axo = false, en = false;
  for (int i = 1; i < argc; i++) {
    if (sscanf(argv[i], "%dx%d", &W, &H) == 2) continue;
    if (!strcmp(argv[i], "round")) round = true;
    else if (!strcmp(argv[i], "axo")) axo = true;
    else if (!strcmp(argv[i], "en")) en = true;
    else dir = argv[i];
  }
  fb.assign(W * H, 0);
  Canvas c(W, H, fb.data());
  cv = &c;
  UiHal hal{hSend, hSpin, hBright, hSave, hRpm, hAngle, hCfg, hZero, hMove};
  Settings s{4.0f, en ? 1 : 0, 80, 16, 200, 6.f, 0, 1.0f, 1, axo ? HOME_AXO : HOME_CARD, 0};
  ui_set_panel(round ? "GC9A01" : "", round);
  ui_init(hal, s, 0);
  line("URL http://192.168.1.42");
  line("HOST spinslicer.local");
  line("NET SpinSlicer-5G");
  frames(1300);
  shot("boot");
  frames(3500);
  shot(axo ? "main_axo" : "main");
  if (axo) {  // Spinny beside the menu: the main page and the service list are enough
    key(K_DOWN);
    key(K_DOWN);
    key(K_OK, 900);
    shot("service_axo");
    return 0;
  }
  key(K_DOWN, 700);
  shot("main_row");
  key(K_UP);
  key(K_OK, 1200);  // Печать -> the videos
  shot("files");
  key(K_DOWN);
  key(K_OK, 900);  // -> setup
  shot("setup");
  key(K_UP);
  key(K_UP);
  key(K_OK, 700);  // edit the correction
  for (int i = 0; i < 4; i++) key(K_UP, 120);
  frames(500);
  shot("edit");
  key(K_OK);
  key(K_DOWN);
  key(K_DOWN, 700);
  line("FILE lattice_cube_1280x800.mp4");
  line("RPM 4");
  line("SPIN 1");
  line("STATE PRINT");
  line("TURNS 6");
  frames(300);
  line("WAIT 3 dark");
  frames(700);
  shot("print_wait");
  line("PROG 42 51");
  line("SYNC 3");
  frames(1500);
  shot("print");
  line("STATE DONE");
  line("SPIN 0");
  frames(1800);
  shot("done");
  key(K_OK, 1200);
  key(K_DOWN);
  key(K_OK, 1000);  // Подключение
  shot("connect");
  key(K_OK, 900);
  key(K_DOWN);
  key(K_OK, 900);  // Сервис
  shot("service");
  key(K_DOWN);
  key(K_OK, 900);  // Мотор
  key(K_OK, 1500);  // Вращение: вкл
  shot("motor");
  key(K_OK);  // off again
  key(K_DOWN);
  key(K_OK, 800);  // edit the speed
  shot("edit_speed");
  key(K_OK);
  key(K_BACK, 900);
  for (int i = 0; i < 4; i++) key(K_DOWN, 200);
  key(K_OK, 1200);  // О принтере
  shot("about");
  key(K_OK, 900);  // back on the service page
  key(K_UP);
  key(K_OK, 900);  // Raspberry Pi
  key(K_DOWN);
  key(K_OK, 700);  // Выключить -> Точно?
  shot("confirm");
  return 0;
}
