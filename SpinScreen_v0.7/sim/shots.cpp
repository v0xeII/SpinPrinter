// Screenshots of every screen at one size, drawn by the real UI (ui.cpp) with a scripted user and
// a fake Pi. Writes one PPM per screen into a folder; dev/make_shots.py puts them on one sheet.
//   ./shots out/          (also: axo = Spinny on the main page instead of the card, en = English)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "gfx.h"
#include "ui.h"

static const int W = 320, H = 240;
static int nowMs = 0, shotN = 0;
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
static void hBeep(int snd, int level) { fprintf(stderr, "beep %d at %d\n", snd, level); }
static void hBuzzer(int pin) { fprintf(stderr, "buzzer on pin %d\n", pin); }
// a Super Mini with the stock wiring: 8, 9, 10 left over (+ the pins it does not break out)
static const uint32_t FREE = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 14) | (1u << 15) | (1u << 16);

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
// a lattice cube, as SpinPi sends the silhouette of a .spin preview (SHAPE lines, 8 rows a line)
static void sendShape() {
  const int W = 56, H = 56, BPR = 7;
  uint8_t bits[H * BPR] = {};
  for (int y = 6; y < 50; y++)
    for (int x = 6; x < 50; x++) {
      int u = (x - 6) % 15, v = (y - 6) % 15;
      bool bar = u < 4 || v < 4 || x >= 46 || y >= 46;
      if (bar) bits[y * BPR + x / 8] |= 0x80 >> (x % 8);
    }
  char b[160];
  snprintf(b, sizeof b, "SHAPE BEGIN %d %d", W, H);
  line(b);
  for (int y0 = 0; y0 < H; y0 += 8) {
    int n = snprintf(b, sizeof b, "SHAPE %d ", y0);
    for (int i = y0 * BPR; i < (y0 + 8) * BPR && i < H * BPR; i++) n += snprintf(b + n, sizeof b - n, "%02X", bits[i]);
    line(b);
  }
  line("SHAPE END");
}
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
  bool axo = false, en = false, light = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "axo")) axo = true;
    else if (!strcmp(argv[i], "en")) en = true;
    else if (!strcmp(argv[i], "light")) light = true;
    else dir = argv[i];
  }
  fb.assign(W * H, 0);
  Canvas c(W, H, fb.data());
  cv = &c;
  UiHal hal{hSend, hSpin, hBright, hSave, hRpm, hAngle, hCfg, hZero, hMove, nullptr, hBeep, hBuzzer, FREE};
  Settings s{4.0f, en ? 1 : 0, 80, 16, 200, 6.f, 0, 1.0f, 1, axo ? HOME_AXO : HOME_CARD, 0};
  s.theme = light ? 1 : 0;
  ui_init(hal, s, 0);
  line("URL http://192.168.1.42");
  line("HOST spinslicer.local");
  line("NET SpinSlicer-5G");
  line("SYS ip=192.168.1.42 wifi=72 temp=48.3 sd=12.4 usb=7.1 prints=14 proj=3.2 hdmi=1");
  frames(1300);
  shot("boot");
  frames(3500);
  shot(axo ? "main_axo" : "main");
  if (axo) {  // Spinny beside the menu: the main page and the settings list are enough
    key(K_DOWN);
    key(K_DOWN);
    key(K_OK, 900);
    shot("settings_axo");
    for (int i = 0; i < 5; i++) key(K_DOWN, 200);
    key(K_OK, 1200);  // Система beside Spinny: the narrow list must still fit
    shot("system_axo");
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
  sendShape();
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
  key(K_OK, 900);  // Настройки: the wizard on top, then the groups
  shot("settings");
  key(K_DOWN, 200);
  key(K_OK, 900);  // Мотор (rotation + the motor's settings)
  key(K_OK, 1500);  // Вращение: вкл
  shot("motor");
  key(K_OK);  // off again
  key(K_DOWN);
  key(K_OK, 800);  // edit the speed
  shot("edit_speed");
  key(K_OK);
  key(K_BACK, 900);  // back to the groups (on Мотор)
  key(K_DOWN, 200);
  key(K_DOWN, 200);
  key(K_OK, 1000);  // Экран: the theme on top
  shot("display");
  key(K_BACK, 900);
  key(K_DOWN, 200);
  key(K_OK, 1000);  // Звук
  key(K_OK, 900);  // Пищалка: the first free pin, and a test note
  shot("sound");
  key(K_BACK, 900);
  key(K_DOWN, 200);
  key(K_OK, 1200);  // Система: the printer's facts + the buttons (the pill starts on "Проверить обновления")
  shot("system");
  key(K_OK, 300);  // -> EV UPDCHECK; the Raspberry answers from GitHub
  line("UPD STEP Скачиваю прошивку 0.8.5…");
  frames(600);
  shot("update_checking");
  line("UPD AVAIL 0.8.5");
  frames(900);
  shot("update_avail");
  key(K_OK, 700);  // Обновить до 0.8.5 -> Точно?
  shot("confirm");
  key(K_OK, 300);  // -> EV UPDATE: first the board's firmware, then the Raspberry itself
  line("FWUP");
  frames(800);
  shot("fw_update");
  line("FWUP END");
  line("UPD RUN pi");
  frames(800);
  shot("pi_update");
  line("UPD IDLE");
  frames(400);
  // the first-setup wizard (Настройки → Мастер настройки)
  key(K_BACK, 900);
  for (int i = 0; i < 5; i++) key(K_UP, 200);
  key(K_OK, 900);
  shot("wiz_hello");
  key(K_OK, 900);  // -> the links
  shot("wiz_link");
  key(K_OK, 600);  // -> focus
  key(K_OK, 600);  // -> centering
  for (int i = 0; i < 3; i++) key(K_UP, 150);
  frames(600);
  shot("wiz_center");
  key(K_OK, 600);  // -> vial width
  key(K_OK, 1800);  // -> direction (the vial turns)
  shot("wiz_dir");
  key(K_OK, 2500);  // -> one exact turn
  shot("wiz_turn");
  key(K_OK, 600);  // -> print speed
  key(K_OK, 600);  // -> screen
  key(K_OK, 900);  // -> done
  shot("wiz_done");
  key(K_OK, 900);
  // a print sent from SpinSlicer: the yes on the printer
  line("ASK 6 214 lattice_cube.spin");
  frames(900);
  shot("ask");
  // on the print screen one ← only asks, the second stops
  key(K_OK, 900);
  line("STATE PRINT");
  line("TURNS 6");
  line("PROG 10 190");
  frames(900);
  key(K_BACK, 600);
  shot("print_stop_ask");
  return 0;
}
