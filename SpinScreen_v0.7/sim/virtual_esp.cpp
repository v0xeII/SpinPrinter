// Virtual ESP32: the real SpinScreen UI (ui.cpp) in a window on a PC, talking to SpinPi over a
// pseudo-terminal exactly like the board does over USB. Lets you test the whole printer
// (screen, buttons, motor, Pi, web page, projector) without any hardware.
//
//   make virtual_esp && ./virtual_esp            first line printed = the serial port for SpinPi
//   ./virtual_esp --headless --inject keys.txt   no window (tests); lines from keys.txt:
//        KEY UP|DOWN|OK|BACK    press a button
//        FAULT freeze|reset|ok  ESP stops answering / reboots / back to normal
//        anything else          sent to the Pi as is (e.g. EV PRINT 0 2 0 1)
//
// Keys in the window: ↑ ↓, Enter/Space = OK, Esc/Backspace = long OK (back),
//                     F = freeze the ESP, R = reboot it.
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "fonts.h"
#include "gfx.h"
#include "spinny.h"
#include "ui.h"

#ifndef NO_SDL
#include <SDL.h>
#endif

using namespace pal;

static double mono() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ============================================================================ motor (as motor.cpp)
// Same rules as the firmware: 5 ms ramp ticks, whole microsteps counted, exact moves.
static struct {
  std::mutex mu;
  int stepsRev = MOTOR_STEPS_PER_REV, micro = MOTOR_MICROSTEPS;
  float gear = MOTOR_GEAR, accel = MOTOR_ACCEL_RPM_S;
  bool dirInv = false;
  float target = 0, current = 0;
  bool wantOn = false, moving = false;
  int dirSign = 1;
  bool dirState = false;
  double pos = 0;  // microsteps (fractional part = the step being made)
  long long moveTarget = 0;
} M;
static double microPerRev() { return M.stepsRev * M.micro * M.gear; }
static long long stepsNow() { return (long long)floor(M.pos); }

static void motorTick(double dt) {  // called every 5 ms
  std::lock_guard<std::mutex> g(M.mu);
  float tgt = M.wantOn ? M.target : 0;
  if (M.moving) {
    float remaining = (float)llabs(M.moveTarget - stepsNow());
    float aSteps = M.accel / 60.f * microPerRev();
    float vMax = sqrtf(2.f * aSteps * remaining) / microPerRev() * 60.f;
    if (vMax < 0.3f) vMax = 0.3f;
    if (tgt > vMax) tgt = vMax;
  }
  if (M.dirInv != M.dirState) tgt = 0;
  float a = M.accel * (float)dt, d = tgt - M.current;
  float cur = M.current + (d > a ? a : (d < -a ? -a : d));
  if (cur < 0.0005f) cur = 0;
  M.current = cur;
  if (cur == 0 && M.dirInv != M.dirState) {
    M.dirState = M.dirInv;
    M.dirSign = M.dirState ? -1 : 1;
  }
  double before = M.pos;
  M.pos += M.dirSign * cur / 60.0 * microPerRev() * dt;
  if (M.moving && ((M.pos - M.moveTarget) * (before - M.moveTarget) <= 0)) {  // landed on the target step
    M.pos = (double)M.moveTarget;
    M.moving = false;
    M.wantOn = false;
    M.current = 0;
  }
}
static double motorAngle() {
  std::lock_guard<std::mutex> g(M.mu);
  return stepsNow() * 360.0 / microPerRev();
}
static float motorRpm() { return M.current; }
static bool motorAtSpeed() { return M.wantOn && !M.moving && fabsf(M.current - M.target) < 1e-4f; }

// ============================================================================ serial over a pty
static int ptyMaster = -1;
static std::mutex txMu;
static std::atomic<bool> frozen{false};
static std::mutex qMu;
static std::deque<std::string> rxQueue;  // lines for the UI loop
static std::deque<std::string> keyQueue;  // KEY … from the inject file
static std::mutex logMu;
static std::deque<std::string> logLines;
static std::atomic<double> lastPing{-100};
static long rxCount = 0, txCount = 0;

static void addLog(const char* dir, const char* line) {
  if (!strcmp(line, "PING") || !strncmp(line, "PONG", 4) || !strcmp(line, "POS") || !strncmp(line, "POS ", 4) ||
      !strcmp(line, "OK"))
    return;
  fprintf(stderr, "%s %s\n", dir, line);
  std::lock_guard<std::mutex> g(logMu);
  logLines.push_back(std::string(dir) + " " + line);
  while (logLines.size() > 9) logLines.pop_front();
}

static void sendLine(const char* line) {
  if (frozen) return;
  std::lock_guard<std::mutex> g(txMu);
  std::string s = std::string(line) + "\n";
  const char* p = s.data();
  size_t n = s.size();
  double t0 = mono();
  while (n > 0 && mono() - t0 < 0.2) {  // nobody reading: drop it instead of hanging the UI
    ssize_t w = write(ptyMaster, p, n);
    if (w > 0) { p += w; n -= w; continue; }
    if (w < 0 && errno != EAGAIN && errno != EINTR) break;
    usleep(500);
  }
  txCount++;
  addLog("ESP>", line);
}

static void serialThread() {
  std::string buf;
  char tmp[512];
  for (;;) {
    pollfd p = {ptyMaster, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0) continue;
    ssize_t r = read(ptyMaster, tmp, sizeof tmp);
    if (r <= 0) { usleep(10000); continue; }
    if (frozen) continue;  // a hung ESP: everything is lost
    buf.append(tmp, r);
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      rxCount++;
      if (line == "POS") {  // answered right here, like serialTask() on the board
        char out[64];
        snprintf(out, sizeof out, "POS %.4f %.4f %d", motorAngle(), (double)motorRpm(), motorAtSpeed() ? 1 : 0);
        sendLine(out);
        continue;
      } else if (line == "ZERO") {
        { std::lock_guard<std::mutex> g(M.mu); M.pos = 0; }
        sendLine("ZEROED");
        continue;
      } else if (!line.compare(0, 9, "SETANGLE ")) {
        { std::lock_guard<std::mutex> g(M.mu); M.pos = llround(atof(line.c_str() + 9) / 360.0 * microPerRev()); }
        sendLine("ANGLE");
        continue;
      }
      if (line == "PING") lastPing = mono();
      addLog("PI >", line.c_str());
      std::lock_guard<std::mutex> g(qMu);
      rxQueue.push_back(line);
    }
  }
}

static void injectThread(std::string path) {
  for (;;) {
    usleep(100000);
    FILE* f = fopen(path.c_str(), "r");
    if (!f) continue;
    char line[256];
    std::vector<std::string> ls;
    while (fgets(line, sizeof line, f)) {
      line[strcspn(line, "\r\n")] = 0;
      if (line[0]) ls.push_back(line);
    }
    fclose(f);
    unlink(path.c_str());
    for (auto& l : ls) {
      std::lock_guard<std::mutex> g(qMu);
      keyQueue.push_back(l);
    }
  }
}

// ============================================================================ UI hardware hooks
static bool EN = false;  // --lang=en: English UI and window texts
#define TR(ru, en) (EN ? (en) : (ru))
static Settings S0{RPM_DEFAULT, 0, 80, MOTOR_MICROSTEPS, MOTOR_STEPS_PER_REV, MOTOR_ACCEL_RPM_S, 0, MOTOR_GEAR, 1, HOME_CARD, 0};
static int brightness = 80;
static void hSend(const char* l) { sendLine(l); }
static void hSpin(bool on, float rpm) {
  std::lock_guard<std::mutex> g(M.mu);
  M.moving = false;
  M.wantOn = on;
  M.target = rpm;
}
static void hBright(int pct) { brightness = pct; }
static void hSave(const Settings& s) { S0 = s; }
static float hRpm() { return motorRpm(); }
static double hAngle() { return motorAngle(); }
static void hCfg(const Settings& s) {
  double ang = motorAngle();
  std::lock_guard<std::mutex> g(M.mu);
  M.stepsRev = s.stepsRev;
  M.micro = s.micro;
  M.gear = s.gear;
  M.accel = s.accel;
  M.dirInv = s.dirInv != 0;
  M.pos = floor(ang / 360.0 * microPerRev());
}
static void hZero() { std::lock_guard<std::mutex> g(M.mu); M.pos = 0; }
static void hMove(double deg, float rpm) {
  std::lock_guard<std::mutex> g(M.mu);
  long long n = llround(fabs(deg) / 360.0 * microPerRev());
  M.moveTarget = stepsNow() + (M.dirInv ? -1 : 1) * n;
  M.moving = true;
  M.target = rpm;
  M.wantOn = true;
}
static UiHal HAL{hSend, hSpin, hBright, hSave, hRpm, hAngle, hCfg, hZero, hMove};

static void reboot(uint32_t ms) {
  {
    std::lock_guard<std::mutex> g(M.mu);
    M.wantOn = false;
    M.moving = false;
    M.current = 0;
    M.pos = 0;
  }
  ui_init(HAL, S0, ms);
  sendLine("READY SpinScreen " FW_VERSION " (virtual)");
}

// ============================================================================ the window
struct Btn { float x, y, r; Key k; const char* label; float lit; };

static void chevron(Canvas& c, float x, float y, float s, bool up, uint16_t col) {
  float d = up ? -1 : 1;
  c.line(x - s, y - d * s * 0.5f, x, y + d * s * 0.5f, 3.2f, col, 1);
  c.line(x, y + d * s * 0.5f, x + s, y - d * s * 0.5f, 3.2f, col, 1);
}

static void drawChrome(Canvas& c, int sx, int sy, int sw, int sh, Btn* b, int nb, const char* tty, bool piOk,
                       double t) {
  c.clear(BG);
  int W = c.W, H = c.H;
  // the printer's front panel
  c.fillRoundRect(sx - 14, sy - 14, sw + 28, sh + 28 + 112, 22, PANEL);
  c.strokeRoundRect(sx - 14, sy - 14, sw + 28, sh + 28 + 112, 22, 1.5f, LINE);
  c.fillRoundRect(sx - 4, sy - 4, sw + 8, sh + 8, 8, hex(0x000000));
  for (int i = 0; i < nb; i++) {
    Btn& k = b[i];
    uint16_t ring = lerpColor(LINE, HOT, k.lit);
    if (k.lit > 0.01f) c.glow(k.x, k.y, k.r + 14, HOT, 0.35f * k.lit);
    c.fillCircle(k.x, k.y, k.r, lerpColor(DARK, hex(0x3A1426), k.lit));
    c.ring(k.x, k.y, k.r, 2.2f, ring);
    uint16_t ic = lerpColor(PINK, WHITE, k.lit);
    if (k.k == K_UP) chevron(c, k.x, k.y, 10, true, ic);
    else if (k.k == K_DOWN) chevron(c, k.x, k.y, 10, false, ic);
    else c.textCentered(F_MD, (int)k.x, (int)k.y + 7, "OK", ic);
    c.textCentered(F_SM, (int)k.x, (int)(k.y + k.r + 22), k.label, DIM);
  }
  // side panel: motor + link
  int px = sx + sw + 44, pw = W - px - 24;
  c.fillRoundRect(px, 24, pw, H - 48, 18, PANEL);
  c.text(F_MD, px + 18, 54, TR("Виртуальный ESP32", "Virtual ESP32"), LIGHT);
  c.text(F_SM, px + 18, 76, tty, DIM);
  float cx = px + 70, cy = 150, R = 50;
  double ang = motorAngle();
  float rpm = motorRpm();
  c.fillCircle(cx, cy, R, DARK);
  c.ring(cx, cy, R, 2, LINE);
  for (int i = 0; i < 12; i++) {
    float a = i * 0.5236f;
    c.line(cx + cosf(a) * (R - 8), cy + sinf(a) * (R - 8), cx + cosf(a) * (R - 3), cy + sinf(a) * (R - 3), 1.5f, FAINT, 1);
  }
  float a = (float)(fmod(ang, 360.0) * M_PI / 180.0) - 1.5708f;
  c.line(cx, cy, cx + cosf(a) * (R - 12), cy + sinf(a) * (R - 12), 4, HOT, 1);
  c.fillCircle(cx, cy, 6, PINK);
  char b2[96];
  snprintf(b2, sizeof b2, "%.2f", (double)rpm);
  c.text(F_LG, px + 140, 146, b2, rpm > 0.01f ? PINK : DIM);
  c.text(F_SM, px + 140, 168, TR("об/мин флакона", "vial rpm"), DIM);
  snprintf(b2, sizeof b2, TR("угол %.1f°", "angle %.1f°"), fmod(ang, 360.0) < 0 ? fmod(ang, 360.0) + 360 : fmod(ang, 360.0));
  c.text(F_SM, px + 140, 190, b2, DIM);
  int y = 236;
  bool fr = frozen;
  c.fillCircle(px + 24, y - 5, 5, fr ? hex(0xE0A040) : (piOk ? PINK : FAINT));
  c.text(F_SM, px + 38, y, fr ? TR("ESP завис (F: отпустить)", "ESP frozen (F: release)") : (piOk ? TR("Pi на связи", "Pi connected") : TR("Pi не подключена", "no Pi")), LIGHT);
  snprintf(b2, sizeof b2, TR("строк: принято %ld, отправлено %ld", "lines: %ld in, %ld out"), rxCount, txCount);
  c.text(F_SM, px + 18, y + 22, b2, DIM);
  c.line(px + 18, y + 38, px + pw - 18, y + 38, 1, LINE, 1);
  y += 62;
  {
    std::lock_guard<std::mutex> g(logMu);
    for (auto& l : logLines) {
      std::string s = l.size() > 40 ? l.substr(0, 39) + "…" : l;
      c.text(F_SM, px + 18, y, s.c_str(), l[0] == 'P' ? LIGHT : PINK);
      y += 19;
    }
  }
  c.text(F_SM, px + 18, H - 62, TR("↑ ↓  Enter = OK   Esc = назад", "↑ ↓  Enter = OK   Esc = back"), DIM);
  c.text(F_SM, px + 18, H - 40, TR("F = ESP завис   R = перезагрузка", "F = freeze ESP   R = reboot"), DIM);
  (void)t;
}

// the whole window (panel + scaled screen) as a picture, for README screenshots: works headless too
static void saveShot(const char* path, const std::vector<uint16_t>& chrome, int WW, int WH, const std::vector<uint16_t>& fb,
                     int W, int H, int sx, int sy, int scale) {
  std::vector<unsigned char> rgb(WW * WH * 3);
  auto put = [&](int i, uint16_t p, int k) {
    rgb[i * 3] = (unsigned char)((((p >> 11) << 3) | (p >> 13)) * k / 255);
    rgb[i * 3 + 1] = (unsigned char)(((((p >> 5) & 63) << 2) | ((p >> 9) & 3)) * k / 255);
    rgb[i * 3 + 2] = (unsigned char)((((p & 31) << 3) | ((p >> 2) & 7)) * k / 255);
  };
  for (int i = 0; i < WW * WH; i++) put(i, chrome[i], 255);
  int k = 60 + brightness * 195 / 100;
  for (int y = 0; y < H * scale; y++)
    for (int x = 0; x < W * scale; x++) put((sy + y) * WW + sx + x, fb[(y / scale) * W + x / scale], k);
  FILE* f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6 %d %d 255\n", WW, WH);
  fwrite(rgb.data(), 1, rgb.size(), f);
  fclose(f);
  fprintf(stderr, "shot: %s\n", path);
}

// ============================================================================ main
int main(int argc, char** argv) {
  const int W = 320, H = 240;
  int scale = 3;
  bool headless = false;
  std::string inject, link, shot;
  double shotAt = 5;
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8);
    else if (!strcmp(argv[i], "--headless")) headless = true;
    else if (!strcmp(argv[i], "--lang=en")) { EN = true; S0.lang = 1; }
    else if (!strcmp(argv[i], "--inject") && i + 1 < argc) inject = argv[++i];
    else if (!strcmp(argv[i], "--link") && i + 1 < argc) link = argv[++i];
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--shot-at") && i + 1 < argc) shotAt = atof(argv[++i]);
    else {
      fprintf(stderr, "usage: virtual_esp [--scale=3] [--headless] [--lang=en] [--inject FILE] [--link PATH] [--shot FILE --shot-at S]\n");
      return 2;
    }
  }
#ifdef NO_SDL
  headless = true;
#endif
  // the "USB cable": a pseudo-terminal, raw mode so nothing echoes back
  ptyMaster = posix_openpt(O_RDWR | O_NOCTTY);
  if (ptyMaster < 0 || grantpt(ptyMaster) || unlockpt(ptyMaster)) { perror("pty"); return 1; }
  const char* slave = ptsname(ptyMaster);
  int sfd = open(slave, O_RDWR | O_NOCTTY);  // kept open so the port never "disappears" between Pi restarts
  termios tio;
  tcgetattr(sfd, &tio);
  cfmakeraw(&tio);
  tcsetattr(sfd, TCSANOW, &tio);
  fcntl(ptyMaster, F_SETFL, fcntl(ptyMaster, F_GETFL) | O_NONBLOCK);
  if (!link.empty()) { unlink(link.c_str()); if (symlink(slave, link.c_str())) perror("link"); }
  printf("%s\n", slave);
  fflush(stdout);

  std::thread(serialThread).detach();
  if (!inject.empty()) std::thread(injectThread, inject).detach();
  std::thread([] {  // motor: 5 ms ticks like the firmware's ramp timer
    double next = mono();
    for (;;) {
      next += 0.005;
      double d = next - mono();
      if (d > 0) usleep((useconds_t)(d * 1e6));
      motorTick(0.005);
    }
  }).detach();

  std::vector<uint16_t> fb(W * H);
  Canvas c(W, H, fb.data());
  double t0 = mono();
  auto nowMs = [&] { return (uint32_t)((mono() - t0) * 1000); };
  ui_init(HAL, S0, nowMs());
  sendLine("READY SpinScreen " FW_VERSION " (virtual)");

  int sw = W * scale, sh = H * scale, sx = 40, sy = 40;
  int WW = sx + sw + 44 + 330, WH = sy + sh + 14 + 112 + 30;
  if (WH < 520) WH = 520;
  std::vector<uint16_t> cb(WW * WH);
  Canvas cc(WW, WH, cb.data());
  Btn btn[3] = {{sx + sw / 2.f - 110, sy + sh + 60.f, 26, K_UP, TR("вверх", "up"), 0},
                {sx + sw / 2.f, sy + sh + 60.f, 30, K_OK, TR("OK · держать = назад", "OK · hold = back"), 0},
                {sx + sw / 2.f + 110, sy + sh + 60.f, 26, K_DOWN, TR("вниз", "down"), 0}};
#ifndef NO_SDL
  SDL_Window* win = nullptr;
  SDL_Renderer* ren = nullptr;
  SDL_Texture *scr = nullptr, *chrome = nullptr;
  if (!headless) {
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "sdl: %s (use --headless)\n", SDL_GetError()); return 1; }
    win = SDL_CreateWindow("SpinScreen: виртуальный ESP32", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, WW, WH,
                           SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, WW, WH);
    scr = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);
    chrome = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, WW, WH);
  }
  double okDown = -1;  // OK held: short = OK, long = back (like the real button)
#endif

  double next = mono();
  for (;;) {
    uint32_t ms = nowMs();
    std::deque<std::string> lines, keys;
    {
      std::lock_guard<std::mutex> g(qMu);
      lines.swap(rxQueue);
      keys.swap(keyQueue);
    }
    for (auto& l : lines) ui_line(l.c_str(), ms);
    for (auto& k : keys) {
      if (!k.compare(0, 4, "KEY ")) {
        std::string n = k.substr(4);
        Key kk = n == "UP" ? K_UP : n == "DOWN" ? K_DOWN : n == "OK" ? K_OK : n == "BACK" ? K_BACK : K_NONE;
        fprintf(stderr, "key %s\n", n.c_str());
        ui_key(kk, ms);
      } else if (k == "FAULT freeze") { frozen = true; fprintf(stderr, "fault: frozen\n"); }
      else if (k == "FAULT ok") { frozen = false; fprintf(stderr, "fault: cleared\n"); }
      else if (k == "FAULT reset") { frozen = false; reboot(ms); }
      else sendLine(k.c_str());
    }
#ifndef NO_SDL
    if (!headless) {
      auto press = [&](Key k) {
        if (frozen) return;
        ui_key(k, ms);
        for (auto& b : btn) if (b.k == k || (k == K_BACK && b.k == K_OK)) b.lit = 1;
      };
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
          switch (e.key.keysym.sym) {
            case SDLK_UP: press(K_UP); break;
            case SDLK_DOWN: press(K_DOWN); break;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: press(K_OK); break;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: press(K_BACK); break;
            case SDLK_f: frozen = !frozen; break;
            case SDLK_r: frozen = false; reboot(ms); break;
            default: break;
          }
        }
        if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
          for (auto& b : btn) {
            float dx = e.button.x - b.x, dy = e.button.y - b.y;
            if (dx * dx + dy * dy > (b.r + 6) * (b.r + 6)) continue;
            if (b.k != K_OK) { if (e.type == SDL_MOUSEBUTTONDOWN) press(b.k); }
            else if (e.type == SDL_MOUSEBUTTONDOWN) okDown = mono();
            else if (okDown > 0) { press(mono() - okDown > 0.6 ? K_BACK : K_OK); okDown = -1; }
          }
        }
      }
      if (okDown > 0 && mono() - okDown > 0.6) { press(K_BACK); okDown = -1; }
    }
#endif
    ui_frame(c, ms, 0);
    bool wantShot = !shot.empty() && mono() - t0 >= shotAt;
    if (!headless || wantShot) {
      for (auto& b : btn) b.lit *= 0.82f;
      bool piOk = mono() - lastPing < 3.0;
      drawChrome(cc, sx, sy, sw, sh, btn, 3, slave, piOk, mono() - t0);
    }
    if (wantShot) {
      saveShot(shot.c_str(), cb, WW, WH, fb, W, H, sx, sy, scale);
      shot.clear();
    }
#ifndef NO_SDL
    if (!headless) {
      SDL_UpdateTexture(chrome, nullptr, cb.data(), WW * 2);
      SDL_UpdateTexture(scr, nullptr, fb.data(), W * 2);
      Uint8 v = (Uint8)(60 + brightness * 195 / 100);
      SDL_SetTextureColorMod(scr, v, v, v);
      SDL_RenderCopy(ren, chrome, nullptr, nullptr);
      SDL_Rect d = {sx, sy, sw, sh};
      SDL_RenderCopy(ren, scr, nullptr, &d);
      SDL_RenderPresent(ren);
    }
#endif
    next += 1.0 / 60;
    double d = next - mono();
    if (d > 0) usleep((useconds_t)(d * 1e6));
    else if (d < -0.1) next = mono();
  }
}
