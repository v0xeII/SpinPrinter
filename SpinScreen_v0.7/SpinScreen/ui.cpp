// Arduino builds with -Os (small code). The drawing code is the hot path, so this file is
// compiled for speed instead.
#pragma GCC optimize("O2")
// SpinScreen UI. Everything on the screen is drawn here, every frame.
//
//   Печать ──► список видео (флешка + загруженные) ──► подготовка: скорость, экспозиция,
//              пауза до света + розовая кнопка ПЕЧАТЬ ──► экран печати ──► Готово!
//   Подключение ──► QR-код страницы принтера
//   Сервис ──► Проектор / Мотор / Экран / Raspberry Pi / О принтере
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "fonts.h"
#include "lang.h"
#include "qrcodegen.h"
#include "spinny.h"
#include "board.h"
#include "buzzer.h"

// this board's wiring, as text. The Raspberry asks for it (PINS) and finds the same string inside a
// firmware file: it only offers an update built for these very pins (a release built for the stock
// wiring would leave a board with other pins without a picture).
#define SS_(x) #x
#define SS(x) SS_(x)
static const char PIN_SIG[] = "SPINPINS " SS(PIN_TFT_SCL) "," SS(PIN_TFT_SDA) "," SS(PIN_TFT_RES) "," SS(PIN_TFT_DC) ","
    SS(PIN_TFT_CS) "," SS(PIN_TFT_BLK) "," SS(TFT_ROTATION) "," SS(TFT_SPI_MHZ) "," SS(TFT_SPI_MODE) "," SS(TFT_INVERT) ","
    SS(TFT_BGR) "," SS(PIN_JOY_X) "," SS(PIN_JOY_Y) "," SS(PIN_JOY_SW) "," SS(JOY_SWAP_XY) "," SS(JOY_INVERT_Y) ","
    SS(JOY_INVERT_X) "," SS(PIN_STEP) "," SS(PIN_DIR) "," SS(PIN_EN);
// release = takes the wiring the board saved (any board can get it over the air); local = sets it
static const char BUILD_TAG[] = "SPINBUILD " SPIN_BUILD;
#include "logo.h"

using namespace pal;

// ================================================================ animation helpers
struct Ease {  // exponential approach: smooth, never overshoots
  float v = 0, target = 0;
  void step(float dt, float speed) { v += (target - v) * (1.f - expf(-dt * speed)); }
};
struct Spring {  // springy approach with a small natural overshoot
  float v = 0, vel = 0, target = 0;
  void step(float dt, float k = 320.f, float damp = 24.f) {
    while (dt > 0) {
      float h = dt > 0.008f ? 0.008f : dt;
      vel += (k * (target - v) - damp * vel) * h;
      v += vel * h;
      dt -= h;
    }
  }
  void snap(float x) { v = target = x; vel = 0; }
};
static float easeOut(float x) {
  x = clampf(x, 0, 1);
  float u = 1 - x;
  return 1 - u * u * u;
}

// ================================================================ menu model
enum ItemId {
  I_PRINT, I_CONNECT, I_SETTINGS,
  I_P_BLACK, I_P_WHITE, I_P_GRID, I_P_FOCUS, I_P_CIRCLE, I_P_CENTER, I_P_VIAL,
  I_ROT, I_SPEED, I_DIR, I_ACCEL, I_MICRO, I_STEPS, I_GEAR, I_ONEREV, I_ZERO,
  I_BRIGHT, I_LANG, I_DARK,
  I_REBOOT, I_POWEROFF,
  I_AXO_SHOW, I_AXO_COLOR,
  I_SU_TURNS, I_SU_FACTOR, I_SU_DELAY,  // print setup rows
  I_G_MOTOR, I_G_DISPLAY, I_G_PROJ, I_G_SYSTEM, I_G_WIZARD,  // the sub-menus of Settings (+ the wizard)
  // System: facts, not buttons (keep them together: infoRow() is a range)
  I_INF_VER, I_INF_RASP, I_INF_NET, I_INF_SPACE, I_INF_PRINTS, I_INF_PROJ, I_INF_BOARD,
  I_FWUP, I_SLEEP, I_UPD,
  I_THEME, I_SOUND, I_CLICKS, I_BUZZ, I_G_SOUND,
  I_FILE,                          // a video in the list
  I_NONE,                          // past the end of a page
};
static const Str ITEM_NAME[] = {
    S_PRINT, S_CONNECT, S_SERVICE,
    S_P_BLACK, S_P_WHITE, S_P_GRID, S_P_FOCUS, S_P_CIRCLE, S_P_CENTER, S_P_VIAL,
    S_ROTATION, S_SPEED, S_DIRECTION, S_ACCEL, S_MICROSTEP, S_STEPS_REV, S_GEAR, S_ONEREV, S_ZERO,
    S_BRIGHT, S_LANG, S_DARK,
    S_REBOOT, S_POWEROFF,
    S_AXO_SHOW, S_AXO_COLOR,
    S_SU_TURNS, S_SU_FACTOR, S_SU_DELAY,
    S_G_MOTOR, S_G_DISPLAY, S_G_PROJ, S_G_SYSTEM, S_G_WIZARD,
    S_VERSION, S_PI, S_NET, S_SPACE, S_PRINTS, S_PROJECTOR, S_BOARD,
    S_FWUP, S_SLEEP, S_UPD_CHECK,
    S_THEME, S_SOUND, S_CLICKS, S_BUZZER, S_G_SOUND,
    S_FILES,
    S_BACK,
};
enum PageId { PG_MAIN, PG_SETTINGS, PG_MOTOR, PG_DISPLAY, PG_PROJ, PG_SYSTEM, PG_SOUND, PG_FILES };
static const ItemId MAIN_ITEMS[] = {I_PRINT, I_CONNECT, I_SETTINGS};
// Settings is a short list of groups; every group is its own page (back = tilt left / long press)
// the wizard first: it walks through all the rest
static const ItemId SETTINGS_ITEMS[] = {I_G_WIZARD, I_G_MOTOR, I_G_PROJ, I_G_DISPLAY, I_G_SOUND, I_G_SYSTEM};
// rotation and the motor's own settings are one page: the everyday rows first
static const ItemId MOTOR_ITEMS[] = {I_ROT, I_SPEED, I_DIR, I_ACCEL, I_MICRO, I_STEPS, I_GEAR, I_ONEREV, I_ZERO};
static const ItemId DISPLAY_ITEMS[] = {I_THEME, I_BRIGHT, I_LANG, I_DARK, I_SLEEP, I_AXO_SHOW, I_AXO_COLOR};
// the buzzer first: without a pin there is nothing to hear
static const ItemId SOUND_ITEMS[] = {I_BUZZ, I_SOUND, I_CLICKS};
static const ItemId PROJ_ITEMS[] = {I_P_BLACK, I_P_WHITE, I_P_GRID, I_P_FOCUS, I_P_CIRCLE, I_P_CENTER, I_P_VIAL};
// System shows what the printer is at once (short rows, skipped by the pill), then the buttons.
// "Update firmware" is there only while the Raspberry offers one (FW line).
static const ItemId SYSTEM_ITEMS[] = {I_INF_VER,   I_INF_RASP, I_INF_NET, I_INF_SPACE, I_INF_PRINTS,
                                      I_INF_PROJ,  I_INF_BOARD, I_UPD,    I_FWUP,      I_REBOOT,    I_POWEROFF};
struct PageDef { const ItemId* items; int n; };
#define PG_N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const PageDef PAGES[] = {{MAIN_ITEMS, PG_N(MAIN_ITEMS)},     {SETTINGS_ITEMS, PG_N(SETTINGS_ITEMS)},
                                {MOTOR_ITEMS, PG_N(MOTOR_ITEMS)},   {DISPLAY_ITEMS, PG_N(DISPLAY_ITEMS)},
                                {PROJ_ITEMS, PG_N(PROJ_ITEMS)},     {SYSTEM_ITEMS, PG_N(SYSTEM_ITEMS)},
                                {SOUND_ITEMS, PG_N(SOUND_ITEMS)},   {nullptr, 0}};
static const Str MORPH_NAME[MORPH_COUNT] = {S_C_PINK, S_C_GOLD, S_C_WILD, S_C_BLACK, S_C_BLUE, S_C_RGB};
static const int MICRO_OPTS[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
static const int STEPS_OPTS[] = {200, 400};
static const int SLEEP_OPTS[] = {0, 1, 5, 15, 30};  // minutes; 0 = never

struct PageView {
  int page = PG_MAIN, sel = 0;
  Spring pill;
  Ease scroll;
};

// ================================================================ state
enum ScreenId { SC_BOOT, SC_MENU, SC_SETUP, SC_PRINT, SC_DONE, SC_CONNECT, SC_ASK, SC_WIZ };

static UiHal H;
static Settings S;
// the buzzer, if there is one: buzzer.cpp keeps it rare (one sound at a time, none within 1.5 s)
static void beep(int snd) {
  if (H.beep && S.buzzPin >= 0) H.beep(snd, S.sound);
}
static ScreenId screen = SC_BOOT;
static float now = 0, last = 0, enterT = 0, fps = 60;
static uint32_t bootMs = 0;

static PageView pv, oldPv;
static int stackPage[6], stackSel[6], depth = 0;
static Ease nav;
static int navDir = 0;

static bool projLit = false;  // a test pattern may be on the projector
static bool editing = false;
static ItemId editItem = I_SPEED;
static Spring edit;
static Spring bump;
static int bumpDir = 0;
static Key lastKey = K_NONE;
static float lastKeyT = -1;
static int fast = 0;
static ItemId confirmItem = I_REBOOT;
static Spring confirm;

static Spring spS, squash;

static Ease leftDial;
static Ease spHide;  // Spinny fades out on full-width screens
static Ease cardSel;  // how lit the PRINT card is (selected = 1)
static Ease homeArt;  // the PRINT card on the main page (Кастомизация → Главная → Карточка)
static Ease bootHide; // Spinny waits until the logo is gone
static Mood tempMood = MOOD_IDLE;
static float tempMoodUntil = -1, lastInput = 0;

static char toastText[96] = "";
static float toastUntil = -1;
static Spring toast;

static bool motorOn = false;
static float lastRx = -100;
static bool everRx = false;

static char url[96] = "", host[48] = "", wifi[48] = "", fileName[64] = "";
static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
static bool qrOk = false;

// video list from the Pi
struct FileEntry { char name[64]; float rpm; bool usb; float planRevs; float tol; float revS; bool cadOk; };
static const int MAX_FILES = 32;  // SpinPi sends at most this many
static FileEntry files[MAX_FILES];
static int nFiles = 0, nIncoming = 0;
static bool listing = false;
static int knownVideos = -1;  // from the last list the Pi sent; -1 = not asked yet
static float listAskedT = -10;
static float defFactor = 0, defDelay = 3;
static int defTurns = 5;
// print setup
static int suFile = 0, suSel = 3;
static int suTurns = 5;
static float suFactor = 0, suDelay = 3;  // factor in %
static bool suManual = false;  // turns typed by hand (not from the plan)
static Spring suPill;
static Ease suBtn;
// projector calibration (owned by the Pi, edited here)
static int calOffset = 0, calVial = 400;

static Ease prog;
static int secLeft = -1;
static bool infinite = false;
static int turnsTotal = 0, drops = 0, waitSec = 0;
static char waitKind[8] = "";
static int doneSel = 1;  // Недосвет / Норма / Пересвет
static float syncMs = -1;
static bool stopping = false;
static float stopAskT = -10;  // when ← was pressed once on the print screen: a second ← stops
// a print sent from SpinSlicer waits here for a yes on the printer (ASK from the Pi)
static char askName[64] = "";
static int askTurns = 0, askSec = 0;
static bool askSent = false;

// what the Raspberry tells about itself (SYS line, every ~10 s); -1 / "" = not known
static char sysIp[24] = "";
static int sysWifi = -1;     // signal %, -1 = no Wi-Fi (cable or unknown)
static float sysTemp = -1;   // CPU °C
static float sysSd = -1, sysUsb = -1;  // free GB
static int sysPrints = -1;
static float sysProjH = -1;  // hours the projector was on (HDMI connected)
static int sysHdmi = -1;     // the projector on HDMI: 1 / 0, -1 = not known
static bool sysSeen = false;  // a SYS line came: the Raspberry runs a SpinPi that sends one
static float piSince = -100;  // when the link to the Raspberry came up
// a firmware the Raspberry can put on this board (FW <version> <source>), and the update itself
static char fwVer[16] = "", fwSrc[8] = "";
static bool fwFlashing = false;
// updates over the air: the Raspberry asks GitHub (EV UPDCHECK) and puts the release on itself and
// on this board (EV UPDATE)
enum { UPD_IDLE, UPD_CHECKING, UPD_NONE, UPD_AVAIL, UPD_ERR };
static int updState = UPD_IDLE;
static char updVer[16] = "";
static float updT = -100;     // when the check was asked
static bool updRun = false;   // the Raspberry is updating itself (it restarts on the way)
static float updRunT = 0;

// first-setup wizard: language, focus, centering, vial width, test spin, done
// first-setup wizard: language, the links, the projector (focus, centering, vial), the motor
// (direction, one exact turn, print speed), the screen, then a summary
enum { WZ_HELLO, WZ_LINK, WZ_FOCUS, WZ_CENTER, WZ_VIAL, WZ_DIR, WZ_TURN, WZ_SPEED, WZ_SCREEN, WZ_DONE, WZ_COUNT };
static int wizStep = 0;
static float wizT = 0;  // when the step began
static int wizPat = 0;  // focus picture: target / grid / circle
static const char* const WIZ_PAT[3] = {"focus", "grid", "circle"};

static bool asleep = false;  // backlight off after S.sleepMin idle minutes

// the part's silhouette (SHAPE lines, from the .spin preview): 1 bit a pixel, rows of (w+7)/8 bytes.
// The print screen shows it inside the progress ring, lighting up as the dose builds.
static const int SHAPE_MAX = 64;
static uint8_t shapeBits[SHAPE_MAX * SHAPE_MAX / 8];
static int shapeW = 0, shapeH = 0;
static bool shapeOk = false;

struct Particle { float x, y, vx, vy, r; uint16_t c; };
static Particle conf[48];

// Rows of the lists. Smaller on the 160x128 screen.
static int ROW_H = 30, PILL_H = 26, ROW_PAD = 13, ROW_TY = 6;  // row pitch, pill height, text inset, baseline
static const Font* FR = &F_MD;    // row text
static const Font* FBIG = &F_LG;  // headings
static const Font* FVAL = &F_XL;  // the number on the edit card

// ---- layout of the 320x240 screen: Spinny / dial on the left, the list on the right.
//  Without Spinny (Кастомизация → Главная → Карточка) the list takes his place, animated.
struct Lay {
  int W = 0, H = 0;
  float artX0, artX1, artY0, artY1, artCx, artCy, dialR, spinS;  // Spinny / dial / QR area
  float lx[2], lw[2], ltop[2], lclip[2];                         // list: [0] beside Spinny, [1] without
  int vis[2];
};
static Lay L;
static void layout(int W, int H) {
  L.W = W;
  L.H = H;
  ROW_H = 30; PILL_H = 26; ROW_PAD = 13; ROW_TY = 6;
  FR = &F_MD; FBIG = &F_LG; FVAL = &F_XL;
  L.artX0 = 0; L.artX1 = 122; L.artY0 = 26; L.artY1 = (float)H;
  L.artCx = 62; L.artCy = 26 + (H - 26) * 0.51f; L.dialR = 44; L.spinS = 0.8f;
  L.lx[0] = 122; L.lw[0] = W - 128.f; L.lx[1] = 8; L.lw[1] = W - 16.f;
  L.ltop[0] = L.ltop[1] = 45; L.lclip[0] = L.lclip[1] = 26;
  L.vis[0] = L.vis[1] = (H - 26) / ROW_H;
}
static float LIST_X = 122, LIST_W = 192, LIST_TOP = 45, LIST_CLIP = 26;
static int VISIBLE = 4;
static Ease wide;  // 0 = beside Spinny, 1 = without him

// ---- Spinny's behaviour: swims about, turns round, follows the menu, eats, sleeps, cheers
struct Brain {
  SpinnyBody body;
  float tx = 60, ty = 100, nextWander = 3, kickY = 0;
  bool food = false;
  float fx = 0, fy = 0, fa = 0, nextFood = 30, eatT = -10;
  float nextAir = 45, airT = -10;  // swims up to gulp air now and then
  float startleT = -10, sleepX = -1;
  int startleDir = 1;
  int side = 1;        // which way he faces for the current goal (+1 right, -1 left)
  float x = 0, y = 0;  // body centre (for the glow and the z's)
};
static Brain B;
static uint32_t rnd = 12345;
static float frand(float a, float b) {
  rnd = rnd * 1664525u + 1013904223u;
  return a + (b - a) * ((rnd >> 8) & 0xFFFF) / 65535.f;
}
static float renderMs = 0, pushKB = 0, drawMsAvg = 0;
static int capFps = 60;  // what the board manages: 60, or 30 if a frame takes longer than ~16 ms

static const char* T(Str s) { return STR[S.lang][s]; }

// the hints about the controls are in lang.h: the joystick (↑↓, → = OK, ← = back)
#define HT T
static bool piOk() { return everRx && (now - lastRx) * 1000.f < PI_TIMEOUT_MS; }
// a Raspberry that has been there a while without one SYS line runs an older SpinPi
static bool oldSpinPi() { return piOk() && !sysSeen && now - piSince > 15; }

static void say(const char* txt, float secs = 1.8f) {
  strncpy(toastText, txt, sizeof(toastText) - 1);
  toastText[sizeof(toastText) - 1] = 0;
  // a long text is cut on a letter, never inside one (Cyrillic letters are 2 bytes)
  size_t n = strlen(toastText), i = n;
  if (n == sizeof(toastText) - 1) {
    while (i > 0 && ((unsigned char)toastText[i - 1] & 0xC0) == 0x80) i--;  // i - 1: the last letter's first byte
    if (i > 0) {
      unsigned char c = (unsigned char)toastText[i - 1];
      size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      if (n - (i - 1) < len) toastText[i - 1] = 0;  // that letter did not fit whole
    }
  }
  toastUntil = now + secs;
  toast.target = 1;
}
static void startle();
static void mood(Mood m, float secs) {
  if (m == MOOD_SAD && tempMood != MOOD_SAD) startle();
  tempMood = m;
  tempMoodUntil = now + secs;
}
static void hop(float k = 4.f) { squash.vel += k; }
static void startle() {  // dart away from where he looks
  B.startleT = now;
  B.startleDir = spinnyFacingRight(B.body) ? -1 : 1;
}
static void sendf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void sendf(const char* fmt, ...) {
  char b[120];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  H.send(b);
}

static void go(ScreenId s) {
  ScreenId prev = screen;
  if (s == SC_MENU && (prev == SC_PRINT || prev == SC_DONE)) {  // after a print: back to the main menu
    depth = 0;
    pv = PageView();
    navDir = 0;
  }
  screen = s;
  enterT = now;
  editing = false;
  edit.target = 0;
  if (s == SC_ASK) askSent = false;
  if (s == SC_PRINT) {
    stopping = false;
    stopAskT = -10;
    syncMs = -1;
    drops = 0;
    if (S.dark && !asleep) H.brightness(10);
  } else if (prev == SC_PRINT && !asleep) {
    H.brightness(S.bright);
  }
  if (s == SC_SETUP) { suSel = 3; suPill.snap(3); }
  if (s == SC_DONE) {
    doneSel = 1;
    for (auto& p : conf) {
      p.x = L.W / 2.f + (rand() % 220 - 110);
      p.y = -10 - rand() % 70;
      p.vx = (rand() % 100 - 50) * 0.6f;
      p.vy = (rand() % 60) * 1.f;
      p.r = 1.2f + (rand() % 10) * 0.15f;
      int k = rand() % 3;
      p.c = k == 0 ? PINK : (k == 1 ? HOT : LIGHT);
    }
  }
}

// ================================================================ formatting
static void fmt2(char* b, size_t n, float v) { snprintf(b, n, "%.2f", (double)v); }
static void fmtTime(char* b, size_t n, int s) {
  if (s < 0) { snprintf(b, n, "—"); return; }
  if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(b, n, "%d:%02d", s / 60, s % 60);
}
static float angleNow() {
  double m = fmod(H.motorAngle(), 360.0);
  if (m < 0) m += 360.0;
  return (float)m;
}
// shorten a UTF-8 string with "…" until it fits maxW pixels
static void fitText(const Font& f, const char* src, char* dst, size_t n, int maxW) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
  if (strlen(src) > n - 1) {  // cut by the buffer: never in the middle of a letter
    size_t len = n - 1;
    while (len > 0 && ((uint8_t)dst[len] & 0xC0) == 0x80) len--;
    dst[len] = 0;
  }
  if (Canvas::textWidth(f, dst) <= maxW) return;
  size_t len = strlen(dst);
  while (len > 0) {
    do { len--; } while (len > 0 && ((uint8_t)dst[len] & 0xC0) == 0x80);  // step back one UTF-8 char
    if (len + 4 > n) continue;
    memcpy(dst + len, "…", 4);  // "…" is 3 bytes + NUL
    if (Canvas::textWidth(f, dst) <= maxW) return;
  }
}

// a paragraph in lines of at most w pixels (broken at spaces); returns the y below it
static int wrapText(Canvas& c, const Font& f, int x, int y, int w, int lh, const char* src, uint16_t col, float a,
                    int maxLines = 3) {
  char line[232], word[64];
  line[0] = 0;
  int lines = 0;
  const char* p = src;
  while (*p && lines < maxLines) {
    while (*p == ' ') p++;
    size_t k = 0;
    while (p[k] && p[k] != ' ' && k < sizeof word - 1) k++;
    memcpy(word, p, k);
    word[k] = 0;
    p += k;
    char next[300];
    snprintf(next, sizeof next, "%s%s%s", line, line[0] ? " " : "", word);
    if (line[0] && Canvas::textWidth(f, next) > w) {  // the word goes to the next line
      if (lines == maxLines - 1) {  // the last line: put what is left on it, cut
        char rest[400], fit[96];
        snprintf(rest, sizeof rest, "%s %s%s", line, word, p);
        fitText(f, rest, fit, sizeof fit, w);
        c.text(f, x, y, fit, col, a);
        return y + lh;
      }
      c.text(f, x, y, line, col, a);
      y += lh;
      lines++;
      snprintf(line, sizeof line, "%s", word);
    } else {
      strncpy(line, next, sizeof line - 1);
      line[sizeof line - 1] = 0;
    }
  }
  if (line[0] && lines < maxLines) {
    char fit[96];
    fitText(f, line, fit, sizeof fit, w);
    c.text(f, x, y, fit, col, a);
    y += lh;
  }
  return y;
}

// 7.1 / 12 (Russian: 7,1): one decimal below 10, none above
static void fmtNum(char* b, size_t n, float v) {
  if (v >= 9.95f) snprintf(b, n, "%.0f", (double)v);
  else snprintf(b, n, "%.1f", (double)v);
  if (S.lang == 0)
    for (char* p = b; *p; p++)
      if (*p == '.') *p = ',';
}
static void itemValue(ItemId it, char* b, size_t n) {
  char r[16];
  switch (it) {
    case I_PRINT: snprintf(b, n, "%s", piOk() ? "→" : T(S_PI_NO)); break;
    case I_CONNECT: snprintf(b, n, "%s", qrOk ? "QR" : "—"); break;
    case I_ROT: snprintf(b, n, "%s", motorOn ? T(S_ON) : T(S_OFF)); break;
    case I_SPEED: fmt2(r, sizeof r, S.rpm); snprintf(b, n, "%s", r); break;
    case I_DIR: snprintf(b, n, "%s", S.dirInv ? T(S_CCW) : T(S_CW)); break;
    case I_ACCEL: snprintf(b, n, "%.0f", (double)S.accel); break;
    case I_MICRO: snprintf(b, n, "1/%d", S.micro); break;
    case I_STEPS: snprintf(b, n, "%d", S.stepsRev); break;
    case I_GEAR: snprintf(b, n, "%.2f", (double)S.gear); break;
    case I_ZERO: snprintf(b, n, "%.1f°", (double)angleNow()); break;
    case I_P_CENTER: snprintf(b, n, "%+d %s", calOffset, T(S_PX)); break;
    case I_P_VIAL: snprintf(b, n, "%d %s", calVial, T(S_PX)); break;
    case I_BRIGHT: snprintf(b, n, "%d%%", S.bright); break;
    case I_LANG: snprintf(b, n, "%s", T(S_LANGNAME)); break;
    case I_DARK: snprintf(b, n, "%s", S.dark ? T(S_ON) : T(S_OFF)); break;
    case I_THEME: snprintf(b, n, "%s", T(S.theme ? S_TH_LIGHT : S_TH_DARK)); break;
    case I_SOUND:
      snprintf(b, n, "%s", T(S.sound == SOUND_LOUD ? S_SND_LOUD : (S.sound == SOUND_QUIET ? S_SND_QUIET : S_OFF)));
      break;
    case I_CLICKS: snprintf(b, n, "%s", S.clicks ? T(S_ON) : T(S_OFF)); break;
    case I_BUZZ:
      if (S.buzzPin < 0) snprintf(b, n, "%s", T(S_NONE));
      else snprintf(b, n, T(S_BUZZ_PIN), S.buzzPin);
      break;
    case I_AXO_SHOW: snprintf(b, n, "%s", T(S.home == HOME_AXO ? S_HOME_AXO : S_HOME_CARD)); break;
    case I_AXO_COLOR: snprintf(b, n, "%s", T(MORPH_NAME[S.axoColor % MORPH_COUNT])); break;
    case I_INF_VER:
      if (fwVer[0]) snprintf(b, n, "%s → %s", FW_VERSION, fwVer);
      else snprintf(b, n, "%s", FW_VERSION);
      break;
    case I_INF_RASP:
      if (!piOk()) snprintf(b, n, "%s", T(S_NOT_CONNECTED));
      else if (oldSpinPi()) snprintf(b, n, "%s · %s", T(S_CONNECTED), T(S_RASP_OLD));
      else if (sysTemp >= 0) snprintf(b, n, "%s · %.0f °C", T(S_CONNECTED), (double)sysTemp);
      else snprintf(b, n, "%s", T(S_CONNECTED));
      break;
    case I_INF_NET:
      if (!piOk() || !sysIp[0]) snprintf(b, n, "—");
      else if (sysWifi >= 0) snprintf(b, n, "%s · Wi-Fi %d%%", sysIp, sysWifi);
      else snprintf(b, n, "%s", sysIp);
      break;
    case I_INF_SPACE: {
      char a[16], u[16];
      if (sysSd < 0) { snprintf(b, n, "—"); break; }
      fmtNum(a, sizeof a, sysSd);
      if (sysUsb < 0) { snprintf(b, n, "SD %s %s", a, T(S_GB)); break; }
      fmtNum(u, sizeof u, sysUsb);
      snprintf(b, n, "SD %s · USB %s %s", a, u, T(S_GB));
      break;
    }
    case I_INF_PRINTS:
      if (sysPrints >= 0) snprintf(b, n, "%d", sysPrints);
      else snprintf(b, n, "—");
      break;
    case I_INF_PROJ:
      if (sysHdmi == 0) snprintf(b, n, "%s", T(S_NOT_CONNECTED_M));
      else if (sysProjH >= 0) { fmtNum(r, sizeof r, sysProjH); snprintf(b, n, "%s %s", r, T(S_HOURS)); }
      else snprintf(b, n, "—");
      break;
    // how long the board has been on, and its frame rate (people porting to other boards look here)
    case I_INF_BOARD:  // what the board can draw (a still screen runs at a calm 30 on purpose)
      fmtTime(r, sizeof r, (int)now);
      snprintf(b, n, "%s · %d fps · %.0f ms", r, capFps, (double)renderMs);
      break;
    case I_FWUP: snprintf(b, n, "%s", fwVer); break;
    case I_UPD:
      if (updState == UPD_CHECKING) snprintf(b, n, "…");
      else if (updState == UPD_NONE) snprintf(b, n, "%s", T(S_UPD_NONE));
      else if (updState == UPD_AVAIL) snprintf(b, n, "%s", updVer);
      else if (updState == UPD_ERR) snprintf(b, n, "%s", T(S_UPD_ERR));
      else b[0] = 0;
      break;
    case I_SLEEP:
      if (S.sleepMin > 0) snprintf(b, n, "%d %s", S.sleepMin, T(S_MIN));
      else snprintf(b, n, "%s", T(S_OFF));
      break;
    case I_P_BLACK: case I_P_WHITE: case I_P_GRID: case I_P_FOCUS: case I_P_CIRCLE:
    case I_ONEREV: case I_REBOOT: case I_POWEROFF: b[0] = 0; break;
    default: snprintf(b, n, "→"); break;
  }
}
static bool editable(ItemId it) {
  switch (it) {
    case I_SPEED: case I_ACCEL: case I_MICRO: case I_STEPS: case I_GEAR: case I_BRIGHT:
    case I_P_CENTER: case I_P_VIAL: case I_SU_TURNS: case I_SU_FACTOR: case I_SU_DELAY: return true;
    default: return false;
  }
}
// info rows (System) are shown, not chosen: the pill steps over them
static bool infoRow(ItemId it) { return it >= I_INF_VER && it <= I_INF_BOARD; }
// their pitch: short rows, so all of System (facts + updates + reboot + shut down) fits on the screen.
// (A firmware on a USB stick adds one more button: then the list scrolls a little.)
static int infoH() { return 17; }
// rows that are on a page only some of the time
static bool hiddenRow(ItemId it) { return it == I_FWUP && !fwVer[0]; }
static int pageCount(const PageView& p) {
  if (p.page == PG_FILES) return nFiles > 0 ? nFiles : 1;
  int n = 0;
  for (int i = 0; i < PAGES[p.page].n; i++) n += !hiddenRow(PAGES[p.page].items[i]);
  return n;
}
static ItemId itemAt(const PageView& p, int i) {
  if (p.page == PG_FILES) return I_FILE;
  for (int j = 0; j < PAGES[p.page].n; j++) {
    ItemId it = PAGES[p.page].items[j];
    if (hiddenRow(it)) continue;
    if (i-- == 0) return it;
  }
  return I_NONE;
}
static ItemId curItem() { return itemAt(pv, pv.sel); }
// row geometry: rows are ROW_H high except the info rows. rowEdge = top edge of row i from the
// first row's top; rowPos = its centre, counted like i * ROW_H (the pill and the list use that)
static float rowHt(const PageView& p, int i) { return p.page != PG_FILES && infoRow(itemAt(p, i)) ? infoH() : ROW_H; }
static float rowEdge(const PageView& p, int i) {
  float e = 0;
  for (int j = 0; j < i; j++) e += rowHt(p, j);
  return e;
}
static float rowPos(const PageView& p, int i) { return rowEdge(p, i) + rowHt(p, i) / 2 - ROW_H / 2.f; }
static float pageHt(const PageView& p) { return rowEdge(p, pageCount(p)); }
// the next row the pill may stop on, from sel in direction d; -1 = none
static int nextSel(const PageView& p, int sel, int d) {
  for (int i = sel + d; i >= 0 && i < pageCount(p); i += d)
    if (!infoRow(itemAt(p, i))) return i;
  return -1;
}

// ================================================================ actions
// the driver is not reconfigured under a spinning motor: the change waits for the stop
static bool cfgPending = false;
static void applyMotorCfg() {
  if (motorOn) { cfgPending = true; return; }
  H.motorConfig(S);
  cfgPending = false;
}

static void setMotor(bool on) {
  motorOn = on;
  if (!on && cfgPending) { H.motorConfig(S); cfgPending = false; }
  H.spin(on, S.rpm);
  sendf("EV SPIN %d", on ? 1 : 0);
}
static void pushPage(int page) {
  oldPv = pv;
  stackPage[depth] = pv.page;
  stackSel[depth] = pv.sel;
  depth++;
  pv = PageView();
  pv.page = page;
  if (page != PG_FILES && infoRow(itemAt(pv, 0))) {  // start on the first button, below the facts
    int f = nextSel(pv, 0, 1);
    pv.sel = f < 0 ? 0 : f;
    pv.pill.snap(rowPos(pv, pv.sel));
  }
  nav.v = 0; nav.target = 1; navDir = 1;
  if (page == PG_FILES) { nFiles = 0; listing = true; listAskedT = now; H.send("EV LIST"); }
}
static void popPage() {
  if (depth == 0) return;
  if (projLit) { H.send("EV PATTERN black"); projLit = false; }  // never leave light on by accident
  oldPv = pv;
  depth--;
  pv = PageView();
  pv.page = stackPage[depth];
  pv.sel = stackSel[depth];
  pv.pill.snap(rowPos(pv, pv.sel));
  pv.scroll.v = pv.scroll.target = pv.sel >= VISIBLE ? (pv.sel - VISIBLE + 1) * ROW_H : 0;
  nav.v = 0; nav.target = 1; navDir = -1;
}

static int indexOf(const int* a, int n, int v) {
  for (int i = 0; i < n; i++) if (a[i] == v) return i;
  return 0;
}
static float stepRpm(float v, int dir) {
  static const float STEP[] = {0.01f, 0.05f, 0.25f, 1.0f};
  float st = STEP[fast < 4 ? 0 : (fast < 12 ? 1 : (fast < 24 ? 2 : 3))];
  float r = roundf((v + st * dir) / st) * st;
  return roundf(clampf(r, RPM_MIN, RPM_MAX) * 100.f) / 100.f;
}

static void changeValue(ItemId it, int dir) {
  bool changed = false;
  switch (it) {
    case I_SPEED: {
      float v = stepRpm(S.rpm, dir);
      changed = v != S.rpm;
      S.rpm = v;
      if (changed && motorOn) H.spin(true, S.rpm);
      break;
    }
    case I_SU_TURNS: {
      int v = suTurns + dir * (fast < 6 ? 1 : 5);
      v = v < 1 ? 1 : (v > 999 ? 999 : v);
      changed = v != suTurns;
      suTurns = v;
      suManual = true;
      break;
    }
    case I_SU_FACTOR: {
      float v = clampf(suFactor + dir * (fast < 6 ? 1.f : 5.f), -50, 100);
      changed = v != suFactor;
      suFactor = v;
      FileEntry& f = files[suFile];
      if (changed && f.planRevs > 0 && !suManual) {
        int t = (int)lroundf(f.planRevs * (1 + suFactor / 100.f));
        suTurns = t < 1 ? 1 : t;
      }
      break;
    }
    case I_SU_DELAY: { float v = clampf(suDelay + dir, 0, 60); changed = v != suDelay; suDelay = v; break; }
    case I_ACCEL: {
      float v = clampf(S.accel + dir * (fast < 6 ? 1.f : 5.f), 1, 60);
      changed = v != S.accel;
      S.accel = v;
      if (changed) applyMotorCfg();
      break;
    }
    case I_MICRO: {
      int i = indexOf(MICRO_OPTS, 9, S.micro) + dir;
      if (i >= 0 && i < 9) { S.micro = MICRO_OPTS[i]; changed = true; applyMotorCfg(); }
      break;
    }
    case I_STEPS: {
      int i = indexOf(STEPS_OPTS, 2, S.stepsRev) + dir;
      if (i >= 0 && i < 2) { S.stepsRev = STEPS_OPTS[i]; changed = true; applyMotorCfg(); }
      break;
    }
    case I_GEAR: {
      float v = roundf(clampf(S.gear + dir * (fast < 6 ? 0.01f : 0.1f), 0.1f, 50.f) * 100.f) / 100.f;
      changed = v != S.gear;
      S.gear = v;
      if (changed) applyMotorCfg();
      break;
    }
    case I_BRIGHT: {
      int v = S.bright + 5 * dir;
      v = v < 10 ? 10 : (v > 100 ? 100 : v);
      changed = v != S.bright;
      S.bright = v;
      H.brightness(S.bright);
      break;
    }
    case I_P_CENTER: {
      int v = calOffset + dir * (fast < 6 ? 1 : 10);
      v = v < -400 ? -400 : (v > 400 ? 400 : v);
      changed = v != calOffset;
      calOffset = v;
      if (changed) sendf("EV OFFSET %d", calOffset);
      break;
    }
    case I_P_VIAL: {
      int v = calVial + dir * (fast < 6 ? 2 : 20);
      v = v < 20 ? 20 : (v > 1900 ? 1900 : v);
      changed = v != calVial;
      calVial = v;
      if (changed) sendf("EV VIALW %d", calVial);
      break;
    }
    default: break;
  }
  if (!changed) { hop(-2); return; }
  bumpDir = dir;
  bump.v = 1;
  bump.vel = 0;
  bump.target = 0;
}

static void startEdit(ItemId it) {
  editing = true;
  editItem = it;
  edit.target = 1;
  if (it == I_P_CENTER) { H.send("EV PATTERN center"); projLit = true; }
  if (it == I_P_VIAL) { H.send("EV PATTERN vial"); projLit = true; }
}
static void endEdit() {
  editing = false;
  edit.target = 0;
  if (editItem == I_SU_TURNS || editItem == I_SU_FACTOR || editItem == I_SU_DELAY) return;  // per print
  H.save(S);
  say(T(S_SAVED), 1.2f);
  if (editItem == I_SPEED) { char r[16]; fmt2(r, sizeof r, S.rpm); sendf("EV RPM %s", r); }
}

static void openSetup(int i) {
  suFile = i;
  suFactor = defFactor;
  suDelay = defDelay;
  suManual = false;
  if (files[i].planRevs > 0) {
    int t = (int)lroundf(files[i].planRevs * (1 + suFactor / 100.f));
    suTurns = t < 1 ? 1 : t;
  } else {
    suTurns = defTurns;
  }
  go(SC_SETUP);
}

// ---- the first-setup wizard
static bool wizMotorStep(int st) { return st == WZ_DIR || st == WZ_TURN || st == WZ_SPEED; }
static bool wizNeedsPi(int st) { return st == WZ_FOCUS || st == WZ_CENTER || st == WZ_VIAL; }
static void wizEnter(int st) {
  wizStep = st;
  wizT = now;
  hop();
  // the projector shows what this step is about, and is dark otherwise
  if (st == WZ_FOCUS) { sendf("EV PATTERN %s", WIZ_PAT[wizPat]); projLit = true; }
  else if (st == WZ_CENTER) { H.send("EV PATTERN center"); projLit = true; }
  else if (st == WZ_VIAL) { H.send("EV PATTERN vial"); projLit = true; }
  else if (projLit) { H.send("EV PATTERN black"); projLit = false; }
  // the motor: turning on the direction and speed steps, one exact turn on its own step
  if (st == WZ_DIR || st == WZ_SPEED) { if (!motorOn) setMotor(true); }
  else if (motorOn) setMotor(false);
  if (st == WZ_TURN) { H.motorZero(); H.motorMove(360, S.rpm); }
}
static void startWizard() {
  go(SC_WIZ);
  wizEnter(WZ_HELLO);
}
static void endWizard() {
  if (motorOn) setMotor(false);
  if (projLit) { H.send("EV PATTERN black"); projLit = false; }
  S.wizard = 1;
  H.save(S);
  char r[16];
  fmt2(r, sizeof r, S.rpm);
  sendf("EV RPM %s", r);
  go(SC_MENU);
}
static void wizKey(Key k) {
  if (k == K_OK) {
    H.save(S);
    if (wizStep == WZ_DONE) { endWizard(); mood(MOOD_HAPPY, 2); return; }
    wizEnter(wizStep + 1);
  } else if (k == K_BACK) {
    if (wizStep == WZ_HELLO) endWizard();  // ← on the first step: skip it all (Settings has it again)
    else wizEnter(wizStep - 1);
  } else {
    int d = k == K_UP ? 1 : -1;
    bool pi = piOk();
    switch (wizStep) {
      case WZ_HELLO: S.lang = 1 - S.lang; hop(); break;
      case WZ_FOCUS:
        if (!pi) { hop(-2); break; }
        wizPat = (wizPat + (d > 0 ? 1 : 2)) % 3;
        sendf("EV PATTERN %s", WIZ_PAT[wizPat]);
        hop();
        break;
      case WZ_CENTER: if (pi) changeValue(I_P_CENTER, d); else hop(-2); break;
      case WZ_VIAL: if (pi) changeValue(I_P_VIAL, d); else hop(-2); break;
      case WZ_DIR:  // stop, turn the other way, start again (the driver is not reconfigured while turning)
        setMotor(false);
        S.dirInv = !S.dirInv;
        H.motorConfig(S);
        setMotor(true);
        hop();
        break;
      case WZ_TURN:
        if (k == K_UP) { H.motorZero(); H.motorMove(360, S.rpm); hop(); }
        else hop(-2);
        break;
      case WZ_SPEED: changeValue(I_SPEED, d); break;
      case WZ_SCREEN: changeValue(I_BRIGHT, d); break;
      default: hop(-2); break;
    }
  }
}
static void afterBoot() {
  if (!S.wizard) startWizard();
  else go(SC_MENU);
}

static void activate() {
  ItemId it = curItem();
  hop();
  switch (it) {
    case I_PRINT:
      if (piOk()) pushPage(PG_FILES);
      else { say(T(S_NEED_PI)); mood(MOOD_SAD, 1.8f); }
      break;
    case I_FILE: if (nFiles > 0) openSetup(pv.sel); break;
    case I_CONNECT: go(SC_CONNECT); break;
    case I_SETTINGS: pushPage(PG_SETTINGS); break;
    case I_G_DISPLAY: pushPage(PG_DISPLAY); break;
    case I_G_PROJ: pushPage(PG_PROJ); break;
    case I_G_MOTOR: pushPage(PG_MOTOR); break;
    case I_G_SYSTEM: pushPage(PG_SYSTEM); break;
    case I_G_SOUND: pushPage(PG_SOUND); break;
    case I_THEME: S.theme = !S.theme; H.save(S); break;  // the next frame is drawn in it
    case I_BUZZ: {  // the next free pin, then none: the test note says which one is right
      int p = S.buzzPin;
      for (;;) {
        if (++p >= 32) { p = -1; break; }
        if (H.freePins & (1u << p)) break;
      }
      S.buzzPin = p;
      if (H.buzzer) H.buzzer(p);
      H.save(S);
      if (p >= 0) {
        if (H.beep) H.beep(SND_TEST, S.sound > SOUND_OFF ? S.sound : SOUND_QUIET);
        say(T(S_BUZZ_HINT), 3);
      }
      break;
    }
    case I_SOUND:
      if (S.buzzPin < 0) { say(T(S_BUZZ_FIRST)); break; }
      S.sound = (S.sound + 1) % 3;
      H.save(S);
      if (H.beep) H.beep(SND_TEST, S.sound);
      break;
    case I_CLICKS:
      if (S.buzzPin < 0) { say(T(S_BUZZ_FIRST)); break; }
      S.clicks = !S.clicks;
      H.save(S);
      break;
    case I_G_WIZARD: startWizard(); break;
    case I_SLEEP: {
      int i = (indexOf(SLEEP_OPTS, 5, S.sleepMin) + 1) % 5;
      S.sleepMin = SLEEP_OPTS[i];
      H.save(S);
      say(T(S_SAVED), 1.2f);
      break;
    }
    case I_FWUP: confirmItem = it; confirm.target = 1; break;
    case I_UPD:
      if (updState == UPD_AVAIL) { confirmItem = it; confirm.target = 1; }
      else if (!piOk()) { say(T(S_NEED_PI)); mood(MOOD_SAD, 1.8f); }
      else if (oldSpinPi()) { updState = UPD_ERR; say(T(S_RASP_OLD), 4); mood(MOOD_SAD, 2); }
      else if (updState != UPD_CHECKING) {
        updState = UPD_CHECKING;
        updT = now;
        H.send("EV UPDCHECK");
        say(T(S_UPD_ASK), 60);  // stays until the answer
      }
      break;
    case I_AXO_SHOW: S.home = (S.home + 1) % HOME_COUNT; H.save(S); say(T(S_SAVED), 1.2f); break;
    case I_AXO_COLOR:
      S.axoColor = (S.axoColor + 1) % MORPH_COUNT;
      S.home = HOME_AXO;  // picking a colour brings him back
      H.save(S);
      mood(MOOD_HAPPY, 0.8f);
      break;
    case I_P_BLACK: H.send("EV PATTERN black"); projLit = true; break;
    case I_P_WHITE: H.send("EV PATTERN white"); projLit = true; break;
    case I_P_GRID: H.send("EV PATTERN grid"); projLit = true; break;
    case I_P_FOCUS: H.send("EV PATTERN focus"); projLit = true; break;
    case I_P_CIRCLE: H.send("EV PATTERN circle"); projLit = true; break;
    case I_ROT: setMotor(!motorOn); say(motorOn ? T(S_SPINNING) : T(S_STOPPED)); break;
    case I_DIR: S.dirInv = !S.dirInv; applyMotorCfg(); H.save(S); say(T(S_SAVED), 1.2f); break;
    case I_ONEREV: if (motorOn) setMotor(false); H.motorZero(); H.motorMove(360, S.rpm); break;
    case I_ZERO: H.motorZero(); say(T(S_ZEROED)); break;
    case I_LANG: S.lang = 1 - S.lang; H.save(S); say(T(S_SAVED), 1.2f); break;
    case I_DARK: S.dark = !S.dark; H.save(S); say(T(S_SAVED), 1.2f); break;
    case I_REBOOT: case I_POWEROFF: confirmItem = it; confirm.target = 1; break;
    default:
      if (editable(it)) startEdit(it);
      break;
  }
}

static void startPrint() {
  sendf("EV PRINT %d %d %.0f %.0f", suFile, suTurns, (double)suFactor, (double)suDelay);
  defFactor = suFactor;
  defDelay = suDelay;
  if (files[suFile].planRevs <= 0) defTurns = suTurns;
  snprintf(fileName, sizeof fileName, "%s", files[suFile].name);
  say(T(S_WAIT_PI));
  hop(6);
}

// ================================================================ public API
void ui_init(const UiHal& hal, const Settings& s, uint32_t nowMs) {
  H = hal;
  S = s;
  bootMs = nowMs;
  now = last = 0;
  spS.snap(1.2f);
  B = Brain();
  screen = SC_BOOT;
  enterT = 0;
  H.brightness(S.bright);
}

void ui_key(Key k, uint32_t nowMs) {
  now = (nowMs - bootMs) / 1000.f;
  bool wasSleepy = (now - lastInput) > 60;
  fast = (k == lastKey && now - lastKeyT < 0.17f) ? fast + 1 : 0;
  lastKey = k;
  lastKeyT = now;
  lastInput = now;
  if (k == K_NONE) return;
  if (fwFlashing || updRun) return;  // the Raspberry is writing a new firmware / updating itself
  if (asleep) { asleep = false; H.brightness(S.bright); return; }  // the first touch only wakes the screen
  if (S.clicks) beep(SND_TICK);
  if (screen == SC_BOOT) { afterBoot(); return; }
  if (wasSleepy && screen == SC_MENU) { mood(MOOD_HAPPY, 0.8f); hop(); return; }

  if (confirm.target > 0) {  // "Точно?" dialog
    if (k == K_OK) {
      if (confirmItem == I_UPD) H.send("EV UPDATE");
      else H.send(confirmItem == I_FWUP ? "EV FWUPDATE" : (confirmItem == I_REBOOT ? "EV REBOOT" : "EV POWEROFF"));
      say(T(S_SENT));
    }
    if (k == K_OK || k == K_BACK) confirm.target = 0;
    return;
  }
  if (editing) {
    if (k == K_UP) changeValue(editItem, +1);
    else if (k == K_DOWN) changeValue(editItem, -1);
    else { endEdit(); hop(); }
    return;
  }

  switch (screen) {
    case SC_MENU: {
      if (nav.v < 0.6f && nav.target == 1) break;  // ignore keys mid-slide
      if (k == K_UP || k == K_DOWN) {
        int d = k == K_DOWN ? 1 : -1;
        int n = nextSel(pv, pv.sel, d);
        if (n < 0) { pv.pill.vel += d * 140; B.kickY -= d * 6; break; }
        pv.sel = n;
        B.kickY += d * 14;  // a little dive / climb with the selection
      } else if (k == K_OK) {
        activate();
      } else if (k == K_BACK) {
        popPage();
      }
      break;
    }
    case SC_SETUP: {
      if (k == K_UP || k == K_DOWN) {
        int n = suSel + (k == K_DOWN ? 1 : -1);
        if (n >= 0 && n <= 3) suSel = n;
        else hop(-2);
      } else if (k == K_OK) {
        if (suSel == 3) startPrint();
        else startEdit(suSel == 0 ? I_SU_TURNS : (suSel == 1 ? I_SU_FACTOR : I_SU_DELAY));
      } else if (k == K_BACK) {
        go(SC_MENU);
      }
      break;
    }
    case SC_PRINT:
      // one ← only asks: a bump of the stick (or noise from the motor on its wires) must never
      // end a print. A second ← within 3 s stops it.
      if (k == K_BACK && !stopping) {
        if (now - stopAskT < 3) { stopping = true; H.send("EV STOP"); }
        else { stopAskT = now; say(T(S_STOP_AGAIN), 3); }
      } else if (k == K_OK) {
        say(HT(S_HOLD_STOP), 1.5f);
      }
      break;
    case SC_ASK:
      if (askSent) break;
      if (k == K_OK) { askSent = true; H.send("EV GO"); hop(6); }
      else if (k == K_BACK) { H.send("EV NO"); go(SC_MENU); say(T(S_ASK_NO)); }
      break;
    case SC_DONE:
      // the three answers are a column: ↑↓ choose, → sends the one lit up, ← skips. (They were a
      // row chosen with ↑↓: pushing right to reach "Over" sent "Good" at once.)
      if (k == K_UP || k == K_DOWN) {
        int n = doneSel + (k == K_DOWN ? 1 : -1);
        if (n >= 0 && n <= 2) doneSel = n;
        else hop(-2);
      } else if (k == K_OK) {
        static const char* R[3] = {"under", "ok", "over"};
        sendf("EV RESULT %s", R[doneSel]);
        go(SC_MENU);
      } else {
        go(SC_MENU);
      }
      break;
    case SC_CONNECT: if (k == K_OK || k == K_BACK) go(SC_MENU); break;
    case SC_WIZ: wizKey(k); break;
    default: break;
  }
}

static void copyArg(char* dst, size_t n, const char* src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

void ui_line(const char* line, uint32_t nowMs) {
  now = (nowMs - bootMs) / 1000.f;
  if (!piOk()) piSince = now;  // the link comes up (again)
  lastRx = now;
  if (!everRx) { everRx = true; say(T(S_PI_HELLO)); }
  char b[112];
  // lines that want the screen: count as activity, so a sleeping screen wakes up for them
  if (!strncmp(line, "MSG ", 4) || !strncmp(line, "ASK ", 4) || !strncmp(line, "STATE ", 6) ||
      !strncmp(line, "FWUP", 4) || !strncmp(line, "UPD ", 4))
    lastInput = now;
  if (!strcmp(line, "PING")) {
    H.send("PONG SpinScreen " FW_VERSION);
  } else if (!strcmp(line, "PINS")) {  // the wiring in use; "nvs" = the board keeps it (board.h)
    if (H.wiring) { snprintf(b, sizeof b, "SPINPINS %s nvs", H.wiring); H.send(b); }
    else H.send(PIN_SIG);
    H.send(BUILD_TAG);
    return;
  } else if (!strncmp(line, "SYS ", 4)) {  // SYS ip=.. wifi=.. temp=.. sd=.. usb=.. prints=.. proj=..  (- = unknown)
    char tmp[160];
    copyArg(tmp, sizeof tmp, line + 4);
    for (char* t = strtok(tmp, " "); t; t = strtok(nullptr, " ")) {
      char* v = strchr(t, '=');
      if (!v) continue;
      *v++ = 0;
      bool none = !strcmp(v, "-");
      if (!strcmp(t, "ip")) copyArg(sysIp, sizeof sysIp, none ? "" : v);
      else if (!strcmp(t, "wifi")) sysWifi = none ? -1 : atoi(v);
      else if (!strcmp(t, "temp")) sysTemp = none ? -1 : (float)atof(v);
      else if (!strcmp(t, "sd")) sysSd = none ? -1 : (float)atof(v);
      else if (!strcmp(t, "usb")) sysUsb = none ? -1 : (float)atof(v);
      else if (!strcmp(t, "prints")) sysPrints = none ? -1 : atoi(v);
      else if (!strcmp(t, "proj")) sysProjH = none ? -1 : (float)atof(v);
      else if (!strcmp(t, "hdmi")) sysHdmi = none ? -1 : atoi(v);
    }
    sysSeen = true;
    return;
  } else if (!strncmp(line, "SHAPE ", 6)) {  // SHAPE BEGIN w h · SHAPE <row> <hex of 8 rows> · SHAPE END · SHAPE -
    const char* a = line + 6;
    if (!strncmp(a, "BEGIN ", 6)) {
      int w = 0, h = 0;
      sscanf(a + 6, "%d %d", &w, &h);
      shapeOk = false;
      memset(shapeBits, 0, sizeof shapeBits);
      shapeW = (w > 0 && w <= SHAPE_MAX) ? w : 0;
      shapeH = (h > 0 && h <= SHAPE_MAX) ? h : 0;
    } else if (!strncmp(a, "END", 3)) {
      shapeOk = shapeW > 0 && shapeH > 0;
    } else if (a[0] == '-') {
      shapeOk = false;
    } else if (shapeW > 0) {
      int y = atoi(a);
      const char* hx = strchr(a, ' ');
      int bpr = (shapeW + 7) / 8;
      if (hx && y >= 0 && y < shapeH) {
        hx++;
        for (int i = 0; hx[0] && hx[1] && y * bpr + i < shapeH * bpr; i++, hx += 2) {
          char two[3] = {hx[0], hx[1], 0};
          shapeBits[y * bpr + i] = (uint8_t)strtol(two, nullptr, 16);
        }
      }
    }
    return;
  } else if (!strncmp(line, "FW ", 3)) {  // FW <version> <usb|release> = an update is there · FW - = none
    fwVer[0] = fwSrc[0] = 0;
    if (strcmp(line + 3, "-")) sscanf(line + 3, "%15s %7s", fwVer, fwSrc);
    return;
  } else if (!strncmp(line, "UPD ", 4)) {  // CHECKING · NONE · AVAIL <ver> · ERR <text> · RUN pi · IDLE
    const char* a = line + 4;
    updRun = false;
    // every step restarts the 40 s "no answer" clock: a slow download is not a silent Raspberry
    if (!strncmp(a, "CHECKING", 8)) { updState = UPD_CHECKING; updT = now; }
    else if (!strncmp(a, "STEP ", 5)) { if (updState == UPD_CHECKING) { updT = now; say(a + 5, 60); } }
    else if (!strncmp(a, "NONE", 4)) { updState = UPD_NONE; say(T(S_UPD_LATEST), 3); mood(MOOD_HAPPY, 1); }
    else if (!strncmp(a, "AVAIL ", 6)) {
      updState = UPD_AVAIL;
      copyArg(updVer, sizeof updVer, a + 6);
      snprintf(b, sizeof b, T(S_UPD_FOUND), updVer);
      say(b, 5);
      mood(MOOD_HAPPY, 1.5f);
      hop(5);
    }
    else if (!strncmp(a, "ERR", 3)) { updState = UPD_ERR; say(a[3] ? a + 4 : T(S_UPD_ERR), 4); mood(MOOD_SAD, 2); }
    else if (!strncmp(a, "RUN", 3)) { updRun = true; updRunT = now; confirm.target = 0; }
    else if (!strncmp(a, "IDLE", 4)) { if (updState == UPD_CHECKING || updState == UPD_AVAIL) updState = UPD_IDLE; }
    if (asleep) { asleep = false; H.brightness(S.bright); }
    return;
  } else if (!strncmp(line, "FWUP", 4)) {  // FWUP = the Raspberry starts writing · FWUP END = it is over
    fwFlashing = strcmp(line, "FWUP END") != 0;
    confirm.target = 0;
    if (asleep) { asleep = false; H.brightness(S.bright); }
    return;
  } else if (!strncmp(line, "FL ", 3)) {  // video list
    const char* a = line + 3;
    static bool skipList = false;  // a list pushed while the user sets up a print must not move files[suFile]
    if (!strncmp(a, "BEGIN", 5)) {
      skipList = screen == SC_SETUP;
      if (skipList) return;
      float fac = 0, d = 3;
      int dt = 5;
      sscanf(a + 5, "%d %f %f %d", &nIncoming, &fac, &d, &dt);
      defFactor = fac;
      defDelay = d;
      defTurns = dt;
      nFiles = 0;
    } else if (skipList) {
      if (!strncmp(a, "END", 3)) skipList = false;
    } else if (!strncmp(a, "END", 3)) {
      listing = false;
      knownVideos = nFiles;
    } else {
      int i = 0, usb = 0, cad = 1, used = 0;
      float rpm = 0, plan = -1, tol = -1, revS = 0;
      if (sscanf(a, "%d %f %d %f %f %f %d %n", &i, &rpm, &usb, &plan, &tol, &revS, &cad, &used) >= 7 && i >= 0 &&
          i < MAX_FILES) {
        copyArg(files[i].name, sizeof files[i].name, a + used);
        files[i].rpm = rpm;
        files[i].usb = usb != 0;
        files[i].planRevs = plan;
        files[i].tol = tol;
        files[i].revS = revS;
        files[i].cadOk = cad != 0;
        if (i + 1 > nFiles) nFiles = i + 1;
      }
    }
    return;
  } else if (!strncmp(line, "CAL ", 4)) {
    sscanf(line + 4, "%d %d", &calOffset, &calVial);
    return;
  } else if (!strncmp(line, "RPM ", 4)) {
    S.rpm = roundf(clampf((float)atof(line + 4), RPM_MIN, RPM_MAX) * 10000.f) / 10000.f;
    if (motorOn) H.spin(true, S.rpm);
  } else if (!strncmp(line, "SPIN ", 5)) {
    motorOn = atoi(line + 5) != 0;
    H.spin(motorOn, S.rpm);
    if (!motorOn && cfgPending) { H.motorConfig(S); cfgPending = false; }
  } else if (!strncmp(line, "PROG ", 5)) {
    float p = 0;
    int s = -1;
    sscanf(line + 5, "%f %d", &p, &s);
    infinite = p < 0;
    waitKind[0] = 0;  // the light is on
    prog.target = infinite ? 0 : clampf(p / 100.f, 0, 1);
    secLeft = s;
    if (screen != SC_PRINT) { go(SC_PRINT); prog.v = prog.target; }
  } else if (!strncmp(line, "ASK ", 4)) {  // ASK <turns> <seconds> <name> · ASK END
    const char* a = line + 4;
    if (!strncmp(a, "END", 3)) {
      if (screen == SC_ASK) go(SC_MENU);
      return;
    }
    int used = 0;
    if (sscanf(a, "%d %d %n", &askTurns, &askSec, &used) >= 2) {
      copyArg(askName, sizeof askName, a + used);
      if (screen != SC_ASK) beep(SND_ASK);
      go(SC_ASK);
      mood(MOOD_HAPPY, 1.5f);
      hop(6);
    }
    return;
  } else if (!strncmp(line, "TURNS ", 6)) {
    turnsTotal = atoi(line + 6);
    return;
  } else if (!strncmp(line, "DROPS ", 6)) {
    drops = atoi(line + 6);
    return;
  } else if (!strncmp(line, "WAIT ", 5)) {
    waitSec = 0;
    waitKind[0] = 0;
    sscanf(line + 5, "%d %7s", &waitSec, waitKind);
    if (screen != SC_PRINT) go(SC_PRINT);
    return;
  } else if (!strncmp(line, "SYNC ", 5)) {
    syncMs = (float)atof(line + 5);
    return;
  } else if (!strncmp(line, "STATE ", 6)) {
    const char* st = line + 6;
    if (!strncmp(st, "PRINT", 5)) {
      if (screen != SC_PRINT) { go(SC_PRINT); prog.v = prog.target = 0; secLeft = -1; }
      strcpy(waitKind, "spin");
      waitSec = 0;
    }
    else if (!strncmp(st, "DONE", 4)) { if (screen != SC_DONE) beep(SND_DONE); go(SC_DONE); }
    else if (!strncmp(st, "IDLE", 4)) { if (screen == SC_PRINT) { go(SC_MENU); say(T(S_STOPPED)); } }
    else if (!strncmp(st, "ERROR", 5)) {
      if (screen == SC_PRINT || screen == SC_SETUP || screen == SC_ASK) go(SC_MENU);
      snprintf(b, sizeof b, "%s%s%s", T(S_ERROR), st[5] ? ": " : "", st[5] ? st + 6 : "");
      say(b, 4);
      mood(MOOD_SAD, 4);
      beep(SND_ERR);
    }
  } else if (!strncmp(line, "URL ", 4)) {
    copyArg(url, sizeof url, line + 4);
    uint8_t tmp[sizeof qr];
    qrOk = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 8, qrcodegen_Mask_AUTO, true);
  } else if (!strncmp(line, "HOST ", 5)) {
    copyArg(host, sizeof host, line + 5);
  } else if (!strncmp(line, "NET ", 4)) {
    copyArg(wifi, sizeof wifi, line + 4);
  } else if (!strncmp(line, "FILE ", 5)) {
    copyArg(fileName, sizeof fileName, line + 5);
  } else if (!strncmp(line, "MSG ", 4)) {
    say(line + 4, 3);
  } else if (!strcmp(line, "STATS")) {  // for tuning: frame rate, work per frame, KB over SPI per frame
    snprintf(b, sizeof b, "STATS %.1f fps %.2f ms %.1f KB draw %.2f ms scr %d page %d sel %d", (double)fps,
             (double)renderMs, (double)pushKB, (double)drawMsAvg, (int)screen, pv.page, pv.sel);
    H.send(b);
    return;
  } else if (!strncmp(line, "KEY ", 4)) {  // press a button from the PC: KEY u / d / o / b
    char k = line[4];
    Key kk = k == 'u' ? K_UP : k == 'd' ? K_DOWN : k == 'o' ? K_OK : k == 'b' ? K_BACK : K_NONE;
    if (kk != K_NONE) ui_key(kk, nowMs);
    return;
  } else if (!strcmp(line, "GET")) {
    char r[16];
    fmt2(r, sizeof r, S.rpm);
    snprintf(b, sizeof b, "CFG RPM %s SPIN %d DIR %d MICRO %d STEPS %d GEAR %.2f ACCEL %.0f LANG %s", r,
             motorOn ? 1 : 0, S.dirInv, S.micro, S.stepsRev, (double)S.gear, (double)S.accel, S.lang ? "en" : "ru");
    H.send(b);
    return;
  } else {
    if (line[0]) H.send("ERR unknown command");
    return;
  }
  H.send("OK");
}

// a filled triangle with rounded corners (rr), anti-aliased
static void fillTriangle(Canvas& c, const float* px, const float* py, float rr, uint16_t col, float a) {
  float bx0 = fmn(fmn(px[0], px[1]), px[2]), bx1 = fmx(fmx(px[0], px[1]), px[2]);
  float by0 = fmn(fmn(py[0], py[1]), py[2]), by1 = fmx(fmx(py[0], py[1]), py[2]);
  float X0 = px[0], Y0 = py[0], X1 = px[1], Y1 = py[1], X2 = px[2], Y2 = py[2];
  c.shape(bx0 - rr, by0 - rr, bx1 + rr, by1 + rr, col, a, [=](float x, float y) {
    // signed distance to a triangle (after Inigo Quilez), minus the rounding
    float e0x = X1 - X0, e0y = Y1 - Y0, e1x = X2 - X1, e1y = Y2 - Y1, e2x = X0 - X2, e2y = Y0 - Y2;
    float v0x = x - X0, v0y = y - Y0, v1x = x - X1, v1y = y - Y1, v2x = x - X2, v2y = y - Y2;
    float t0 = clampf((v0x * e0x + v0y * e0y) / (e0x * e0x + e0y * e0y), 0, 1);
    float t1 = clampf((v1x * e1x + v1y * e1y) / (e1x * e1x + e1y * e1y), 0, 1);
    float t2 = clampf((v2x * e2x + v2y * e2y) / (e2x * e2x + e2y * e2y), 0, 1);
    float q0x = v0x - e0x * t0, q0y = v0y - e0y * t0, q1x = v1x - e1x * t1, q1y = v1y - e1y * t1;
    float q2x = v2x - e2x * t2, q2y = v2y - e2y * t2;
    float d = fmn(fmn(q0x * q0x + q0y * q0y, q1x * q1x + q1y * q1y), q2x * q2x + q2y * q2y);
    float sg = e0x * e2y - e0y * e2x > 0 ? 1.f : -1.f;
    float c0 = sg * (v0x * e0y - v0y * e0x), c1 = sg * (v1x * e1y - v1y * e1x), c2 = sg * (v2x * e2y - v2y * e2x);
    bool in = (c0 >= 0 && c1 >= 0 && c2 >= 0) || (c0 <= 0 && c1 <= 0 && c2 <= 0);
    return (in ? -1.f : 1.f) * fsqrt(d) - rr;
  });
}

// ================================================================ logo + main page
// The SpinSlicer icon, from the numbers in assets/spinslicer.svg (a 256-unit tile): a pink tile
// with round corners, a dark vial seen from above, the "S" (a bitmap made by gen/make_logo.py)
// and the arrow round the vial. sz = tile size in px; rot turns the arrow (0 = as in the SVG).
static void drawLogo(Canvas& c, float cx, float cy, float sz, float a, float rot) {
  if (a <= 0.01f || sz < 8) return;
  const uint16_t INK = hex(0x141217);
  float k = sz / 256.f, ox = cx - sz / 2, oy = cy - sz / 2;
  c.fillRoundRect(ox, oy, sz, sz, 56 * k, PINK, a);
  c.fillCircle(cx, cy, 88 * k, INK, a);
  // the S: sample the mask (bilinear) and hand it to shape() as "0.5 - coverage"
  float mk = LOGO_S_SCALE / k;  // mask px per screen px
  float sx0 = ox + LOGO_S_X0 * k, sy0 = oy + LOGO_S_Y0 * k;
  c.shape(sx0, sy0, sx0 + LOGO_S_W / LOGO_S_SCALE * k, sy0 + LOGO_S_H / LOGO_S_SCALE * k, PINK, a,
          [=](float x, float y) {
            float u = (x - sx0) * mk - 0.5f, v = (y - sy0) * mk - 0.5f;
            int iu = ifloor(u), iv = ifloor(v);
            float fu = u - iu, fv = v - iv;
            auto at = [](int i, int j) -> float {
              return (i < 0 || j < 0 || i >= LOGO_S_W || j >= LOGO_S_H) ? 0.f : LOGO_S[j * LOGO_S_W + i];
            };
            float m = (at(iu, iv) * (1 - fu) + at(iu + 1, iv) * fu) * (1 - fv) +
                      (at(iu, iv + 1) * (1 - fu) + at(iu + 1, iv + 1) * fu) * fv;
            return 0.5f - m * (1.f / 255.f);
          });
  // the arrow: an arc (r 106, stroke 8, from -100° to -15°) and a rounded triangle at its end
  c.arc(cx, cy, 106 * k, 8 * k, -1.745f + rot, -0.262f + rot, INK, a);
  const float TX[3] = {234.7f, 241.6f, 219.2f}, TY[3] = {116.8f, 97.6f, 103.6f};
  float px[3], py[3], cr = cosf(rot), sr = sinf(rot);
  for (int i = 0; i < 3; i++) {
    float dx = (TX[i] - 128) * k, dy = (TY[i] - 128) * k;
    px[i] = cx + dx * cr - dy * sr;
    py[i] = cy + dx * sr + dy * cr;
  }
  fillTriangle(c, px, py, 2 * k, INK, a);  // stroke-width 4 with round joins
}

// The card, across the top of the main page (shorter on 170 px high screens).
static void cardRect(float& x, float& y, float& w, float& h) {
  x = 8; y = 32; w = L.W - 16.f; h = 76.f;
}

// Main page with Кастомизация → Главная → Карточка: "Печать" (the first item of the main page) is
// a big card across the top, the list under it shows the rest. sel = how selected the card is
// (0..1, follows the pill as it slides up into the card).
static void drawHomeCard(Canvas& c, float a, float sel) {
  if (a <= 0.01f) return;
  float x, y, w, h;
  cardRect(x, y, w, h);
  x += (1 - a) * -16;
  float rr = 16;
  // no pulse and no glow: a still card costs nothing over SPI (a pulsing one resent ~50 KB a frame)
  c.fillRoundRect(x, y, w, h, rr, lerpColor(PANEL, ACT, sel), a);
  c.strokeRoundRect(x, y, w, h, rr, 1.5f, lerpColor(EDGE, HOT, sel), a);
  uint16_t ink = lerpColor(PINK, DARK, sel), sub = lerpColor(DIM, SUBSEL, sel);
  char b[48], s2[48];
  if (!piOk()) snprintf(b, sizeof b, "%s", T(S_PI_NO));
  else if (knownVideos > 0) snprintf(b, sizeof b, "%s: %d", T(S_FILES), knownVideos);
  else snprintf(b, sizeof b, "%s", T(S_CARD_PICK));
  c.text(F_LG, (int)(x + 18), (int)(y + h * 0.47f), T(S_PRINT_BTN), ink, a);
  fitText(F_SM, b, s2, sizeof s2, (int)w - 70);
  c.text(F_SM, (int)(x + 18), (int)(y + h * 0.78f), s2, sub, a);
  c.text(F_LG, (int)(x + w - 40), (int)(y + h * 0.62f), "→", ink, a);
}

// ================================================================ drawing pieces
static void drawTopBar(Canvas& c) {
  bool ok = piOk();
  const char* pt = ok ? T(S_PI_OK) : T(S_PI_NO);
  float rpm = H.motorRpm();
  bool spin = rpm > 0.005f || motorOn;
  char r[16];
  fmt2(r, sizeof r, rpm);
  c.text(F_SM, 12, 17, "SpinSlicer", PINK);
  int w = Canvas::textWidth(F_SM, pt) + 24;
  int x = L.W - 8 - w;
  c.fillRoundRect(x, 5, w, 17, 8.5f, PANEL);
  c.fillCircle(x + 10, 13.5f, 3.2f, ok ? PINK : FAINT, ok ? 0.6f + 0.4f * sinf(now * 3) : 1);
  c.text(F_SM, x + 17, 18, pt, ok ? LIGHT : DIM);
  if (spin) {
    int mx = x - 12 - Canvas::textWidth(F_SM, r);
    c.text(F_SM, mx, 18, r, LIGHT);
    float a = angleNow() * 0.0174533f;
    c.ring(mx - 10, 13, 5, 1.6f, FAINT);
    c.fillCircle(mx - 10 + cosf(a) * 5, 13 + sinf(a) * 5, 1.9f, PINK);
  }
}

static void drawToast(Canvas& c) {
  if (toast.v < 0.02f || !toastText[0]) return;
  float a = clampf(toast.v, 0, 1);
  char t[64];
  fitText(F_SM, toastText, t, sizeof t, L.W - 40);
  int tw = Canvas::textWidth(F_SM, t);
  float w = tw + 24, h = 20;
  float x = L.W / 2.f - w / 2, y = (3.5f) - (1 - a) * 22;
  if (x < 4) x = 4;
  // the message has the top line to itself: half over the "Raspberry" chip it looked broken
  c.fillRect(0, 0, L.W, 25, BG, a);
  c.fillRoundRect(x, y, w, h, 10, PANEL, a);
  c.strokeRoundRect(x, y, w, h, 10, 1, HOT, a * 0.8f);
  c.text(F_SM, (int)(x + 12), (int)(y + 14.5f), t, LIGHT, a);
}

// Tachometer: top view of the vial, the marker turns with the real motor angle
static void drawDial(Canvas& c, float cx, float cy, float R, float alpha, bool numbers = true) {
  if (alpha < 0.01f) return;
  c.glow(cx, cy, R * 1.6f, HOT, 0.12f * alpha);
  c.fillCircle(cx, cy, R - 6, PANEL, alpha);
  c.ring(cx, cy, R, 2, LINE, alpha);
  for (int i = 0; i < 12; i++) {
    float a = i * 0.5236f - 1.5708f;
    float r0 = i % 3 == 0 ? R - 9 : R - 6.5f;
    c.capsule(cx + cosf(a) * r0, cy + sinf(a) * r0, cx + cosf(a) * (R - 3.5f), cy + sinf(a) * (R - 3.5f),
              i == 0 ? 1.1f : 0.7f, i == 0 ? PINK : FAINT, alpha);
  }
  float ang = angleNow() * 0.0174533f - 1.5708f;
  float rpm = H.motorRpm();
  if (rpm > 0.01f) {
    float trail = clampf(rpm / RPM_MAX * 3.f + 0.35f, 0.35f, 1.6f) * (S.dirInv ? 1 : -1);
    c.arc(cx, cy, R, 2.6f, ang + trail, ang, HOT, alpha * 0.45f);
  }
  float mx = cx + cosf(ang) * R, my = cy + sinf(ang) * R;
  c.glow(mx, my, 10, HOT, 0.8f * alpha);
  c.fillCircle(mx, my, 3.4f, LIGHT, alpha);
  if (numbers && R < 24) {  // too small for the number inside (160x128): beside it
    char r[16];
    fmt2(r, sizeof r, rpm);
    int tx = (int)(cx + R + 10);
    tx = c.text(F_MD, tx, (int)cy + 6, r, LIGHT, alpha);
    c.text(F_SM, tx + 4, (int)cy + 6, T(S_RPM), DIM, alpha);
  } else if (numbers) {
    char r[16];
    fmt2(r, sizeof r, rpm);
    c.textCentered(F_MD, (int)cx, (int)cy + 4, r, LIGHT, alpha);
    c.textCentered(F_SM, (int)cx, (int)cy + 18, T(S_RPM), DIM, alpha);
  }
}

// The edit card: grows from (fx,fy,fw,26) to (tx,ty,tw,th) as e goes 0 -> 1
static void drawEditCard(Canvas& c, float fx, float fy, float fw, float tx, float ty, float tw, float th, float alpha) {
  float e = clampf(edit.v, 0, 1.15f);
  if (e < 0.01f) return;
  float x = fx + (tx - fx) * e, y = fy + (ty - fy) * e, w = fw + (tw - fw) * e, h = PILL_H + (th - PILL_H) * e;
  int ocx0 = c.cx0, ocy0 = c.cy0, ocx1 = c.cx1, ocy1 = c.cy1;
  c.noClip();
  float rr = PILL_H / 2.f + 5 * clampf(e, 0, 1);
  c.fillRoundRect(x, y, w, h, rr, ACT, alpha);
  int txw = (int)w - 12;  // text width
  int t0 = 20;           // the title's baseline below the top
  float ia = alpha * clampf((e - 0.55f) / 0.45f, 0, 1);
  if (ia < 0.01f) { c.clip(ocx0, ocy0, ocx1, ocy1); return; }
  ItemId it = editItem;
  int cx = (int)(x + w / 2);
  c.textCentered(F_SM, cx, (int)(y + t0), T(ITEM_NAME[it]), DARK, ia * 0.7f);
  char v[20], sub[48] = "";
  const char* unit = "";
  const char* hint = HT(S_HINT);
  switch (it) {
    case I_SU_TURNS: {
      FileEntry& f = files[suFile];
      snprintf(v, sizeof v, "%d", suTurns);
      unit = T(S_TURNS_UNIT);
      char t[16];
      fmtTime(t, sizeof t, (int)lroundf(suTurns * f.revS));
      if (f.planRevs > 0) snprintf(sub, sizeof sub, "%s  ·  %s %.2f", t, T(S_PLAN), (double)f.planRevs);
      else snprintf(sub, sizeof sub, "%s", t);
      break;
    }
    case I_SPEED: {
      float r = S.rpm;
      fmt2(v, sizeof v, r);
      unit = T(S_RPM);
      hint = HT(S_HINT_FINE);
      snprintf(sub, sizeof sub, "%.1f %s  ·  %.2f %s", (double)(r * 6.f), T(S_DEG_S), (double)(60.f / r), T(S_SEC_REV));
      break;
    }
    case I_SU_FACTOR: {
      snprintf(v, sizeof v, "%+.0f", (double)suFactor);
      unit = "%";
      char t[16];
      fmtTime(t, sizeof t, (int)lroundf(suTurns * files[suFile].revS));
      snprintf(sub, sizeof sub, "→ %d %s  ·  %s", suTurns, T(S_TURNS_UNIT), t);
      break;
    }
    case I_SU_DELAY: snprintf(v, sizeof v, "%.0f", (double)suDelay); unit = T(S_SEC); break;
    case I_ACCEL: snprintf(v, sizeof v, "%.0f", (double)S.accel); unit = T(S_RPM_S); break;
    case I_MICRO:
      snprintf(v, sizeof v, "1/%d", S.micro);
      snprintf(sub, sizeof sub, "%d %s", (int)(S.stepsRev * S.micro * S.gear), T(S_STEPS));
      break;
    case I_STEPS: snprintf(v, sizeof v, "%d", S.stepsRev); snprintf(sub, sizeof sub, "%.1f°", 360.0 / S.stepsRev); break;
    case I_GEAR:
      snprintf(v, sizeof v, "%.2f", (double)S.gear);
      snprintf(sub, sizeof sub, "%d %s", (int)(S.stepsRev * S.micro * S.gear), T(S_STEPS));
      break;
    case I_P_CENTER: snprintf(v, sizeof v, "%+d", calOffset); unit = T(S_PX); hint = T(S_HINT_LIVE); break;
    case I_P_VIAL: snprintf(v, sizeof v, "%d", calVial); unit = T(S_PX); hint = T(S_HINT_LIVE); break;
    default: snprintf(v, sizeof v, "%d", S.bright); unit = "%"; break;
  }
  float off = bump.v * 14 * bumpDir;
  float va = ia * clampf(1 - fabsf(bump.v) * 0.8f, 0, 1);
  float vy = y + h * 0.52f;  // proportions of the card, so it fits any screen
  c.clip((int)x, (int)y + t0 + 4, (int)(x + w), (int)(y + h * 0.64f));
  c.textCentered(*FVAL, cx, (int)(vy + off), v, DARK, va);
  c.noClip();
  c.textCentered(F_SM, cx, (int)(y + h * 0.65f), unit, DARK, ia * 0.8f);
  char sb[48];
  fitText(F_SM, sub, sb, sizeof sb, txw);
  if (sb[0]) c.textCentered(F_SM, cx, (int)(y + h * 0.78f), sb, DARK, ia * 0.75f);
  fitText(F_SM, hint, sb, sizeof sb, txw);
  c.textCentered(F_SM, cx, (int)(y + h * 0.93f), sb, DARK, ia * 0.5f);
  c.clip(ocx0, ocy0, ocx1, ocy1);
}

static void editTarget(float& x, float& y, float& w, float& h) {
  x = LIST_X; y = 30; w = LIST_W; h = L.H - 36.f;
}

static void drawList(Canvas& c, PageView& p, float dx, float alpha, bool active) {
  float e = active ? clampf(edit.v, 0, 1.15f) : 0;
  float listA = alpha * clampf(1 - e * 1.6f, 0, 1);
  c.clip((int)LIST_X - 6, (int)LIST_CLIP, L.W, L.H);
  int skip = (p.page == PG_MAIN && S.home == HOME_CARD) ? 1 : 0;  // "Печать" is the card
  float listTop = LIST_TOP;
  if (skip) {  // the rest of the main page goes under the card
    float cx, cy, cw, ch;
    cardRect(cx, cy, cw, ch);
    listTop = cy + ch + ROW_H * 0.5f + 8;
    c.clip((int)LIST_X - 6, (int)(cy + ch + 4), L.W, L.H);
  }
  float py = listTop + p.pill.v - skip * ROW_H - p.scroll.v;
  if (p.page == PG_FILES && nFiles == 0) {  // loading / empty / no Pi
    bool loading = listing && now - listAskedT < 15;  // the Pi may be reading a big USB stick
    int x = (int)(LIST_X + 8 + dx);
    float k = ROW_H / 30.f;
    if (loading) {
      c.text(*FR, x, (int)(LIST_TOP + 35 * k), T(S_LOADING), LIGHT, alpha);
      for (int i = 0; i < 3; i++)
        c.fillCircle(x + 6 + i * 12, LIST_TOP + 55 * k, 3, PINK, alpha * (0.3f + 0.7f * (0.5f + 0.5f * sinf(now * 6 - i))));
    } else {
      c.text(*FR, x, (int)(LIST_TOP + 29 * k), T(S_NO_FILES1), LIGHT, alpha);
      char l1[64];
      fitText(F_SM, T(S_NO_FILES2), l1, sizeof l1, (int)LIST_W - 16);
      c.text(F_SM, x, (int)(LIST_TOP + 51 * k), l1, DIM, alpha);
      fitText(F_SM, HT(S_BACK_HINT), l1, sizeof l1, (int)LIST_W - 8);
      c.text(F_SM, x, L.H - 10, l1, FAINT, alpha * 0.7f);
    }
    c.noClip();
    return;
  }
  if (listA > 0.01f) {
    float stretch = clampf(fabsf(p.pill.vel) * 0.012f, 0, 5);
    float pillA = skip ? clampf(p.pill.v / ROW_H, 0, 1) : 1;  // melts into the card on the way up
    c.fillRoundRect(LIST_X + dx, py - PILL_H / 2.f - stretch / 2, LIST_W, PILL_H + stretch, PILL_H / 2.f, ACT, listA * pillA);
    int n = pageCount(p);
    for (int i = skip; i < n; i++) {
      float ry = listTop + rowPos(p, i) - skip * ROW_H - p.scroll.v;
      if (ry < LIST_CLIP - 16 || ry > L.H + 16) continue;
      float k = clampf(1 - fabsf(ry - py) / ROW_H, 0, 1);
      ItemId it = itemAt(p, i);
      int ty = (int)(ry + ROW_TY);
      uint16_t vc = lerpColor(DIM, DARK, k);
      if (it == I_FILE) {
        char v[16], nm[64];
        fmt2(v, sizeof v, files[i].rpm);
        int vw = Canvas::textWidth(F_SM, v);
        int bw = files[i].usb ? Canvas::textWidth(F_SM, T(S_USB)) + 12 : 0;
        fitText(*FR, files[i].name, nm, sizeof nm, (int)LIST_W - 2 * ROW_PAD - 8 - vw - bw);
        c.text(*FR, (int)(LIST_X + ROW_PAD + dx + k * 2), ty, nm, lerpColor(ROWTXT, DARK, k), listA);
        int rx = (int)(LIST_X + LIST_W - ROW_PAD + dx);
        c.textRight(F_SM, rx, ty - 1, v, vc, listA);
        if (files[i].usb) {
          int bx = rx - vw - bw - 2;
          c.fillRoundRect(bx, ty - 12, bw - 4, 15, 7.5f, k > 0.5f ? DARK : LINE, listA);
          c.text(F_SM, bx + 4, ty - 1, T(S_USB), k > 0.5f ? PINK : DIM, listA);
        }
        continue;
      }
      if (infoRow(it)) {  // System: a fact, a dim name and its value (as the old About screen)
        char v[48], fv[48];
        itemValue(it, v, sizeof v);
        const char* nm = T(ITEM_NAME[it]);
        int lx = (int)(LIST_X + ROW_PAD + dx);
        int rx = (int)(LIST_X + LIST_W - ROW_PAD + dx);
        int iy = (int)(ry + 4);  // a short row (infoH): text a little higher, the rule at its bottom
        c.text(F_SM, lx, iy, nm, DIM, listA);
        fitText(F_SM, v, fv, sizeof fv, rx - lx - Canvas::textWidth(F_SM, nm) - 10);
        uint16_t col = (it == I_INF_RASP && piOk()) || (it == I_INF_VER && fwVer[0]) ? PINK : LIGHT;
        c.textRight(F_SM, rx, iy, fv, col, listA);
        c.fillRect(lx, (int)(ry + infoH() / 2.f - 1), rx - lx, 1, LINE, listA);
        continue;
      }
      uint16_t base = it == I_PRINT ? PINK : ROWTXT;
      if (it == I_POWEROFF || it == I_REBOOT) base = WARN;
      char v[32];
      itemValue(it, v, sizeof v);
      const char* nm = T(ITEM_NAME[it]);
      // beside Spinny "Перезагрузить Raspberry" is too long: the row says just the verb
      // (the "Точно?" card still names the Raspberry)
      int room = (int)LIST_W - 2 * ROW_PAD - Canvas::textWidth(F_SM, v) - 6;
      if (it == I_UPD) {  // "Check for updates" → "Update to 0.8.5"; beside Spinny just "Updates"
        if (updState == UPD_AVAIL) nm = T(S_UPD_DO);
        else if (Canvas::textWidth(*FR, nm) > room) nm = T(S_UPD_SHORT);
      }
      if (it == I_FWUP && Canvas::textWidth(*FR, nm) > room) {  // narrow: the Version row has the number
        v[0] = 0;
        room = (int)LIST_W - 2 * ROW_PAD;
      }
      if ((it == I_REBOOT || it == I_POWEROFF) &&  // both or neither, so the pair reads alike
          fmaxf(Canvas::textWidth(*FR, T(S_REBOOT)), Canvas::textWidth(*FR, T(S_POWEROFF))) > room)
        nm = T(it == I_REBOOT ? S_REBOOT_SHORT : S_POWEROFF_SHORT);
      c.text(*FR, (int)(LIST_X + ROW_PAD + dx + k * 2), ty, nm, lerpColor(base, DARK, k), listA);
      if (it == I_UPD && updState == UPD_CHECKING) {  // a turning dot: it is working
        float sx = LIST_X + LIST_W - ROW_PAD - 7 + dx, sy = ry, ang = now * 7;
        uint16_t col = lerpColor(PINK, DARK, k);
        c.ring(sx, sy, 6, 1.6f, lerpColor(FAINT, DARK, k * 0.6f), listA);
        c.fillCircle(sx + cosf(ang) * 6, sy + sinf(ang) * 6, 2.2f, col, listA);
        continue;
      }
      if ((it == I_ROT || it == I_DARK || it == I_AXO_SHOW || it == I_CLICKS || it == I_SOUND) && v[0] &&
          strcmp(v, T(S_OFF))) vc = lerpColor(PINK, DARK, k);
      c.textRight(F_SM, (int)(LIST_X + LIST_W - ROW_PAD + dx), ty - 1, v, vc, listA);
    }
    float view = VISIBLE * ROW_H, all = pageHt(p);
    if (all > view + 0.5f) {  // scroll indicator
      float span = L.H - LIST_CLIP - 8;
      float frac = clampf(p.scroll.v / (all - view), 0, 1);
      float th = span * view / all;
      c.fillRoundRect(L.W - 3 + dx, LIST_CLIP + 4 + (span - th) * frac, 2, th, 1, FAINT, listA);
    }
  }
  if (active) {
    float x, y, w, h;
    editTarget(x, y, w, h);
    drawEditCard(c, LIST_X + dx, py - PILL_H / 2.f, LIST_W, x + dx, y, w, h, alpha);
  }
  c.noClip();
}

static void drawPage(Canvas& c, PageView& p, float dx, float alpha, bool active) {
  drawList(c, p, dx, alpha, active);
}

static void drawMenu(Canvas& c, float dx, float ca) {
  if (nav.v < 0.999f && navDir != 0) {
    float k = easeOut(nav.v);
    float Wd = L.W * 0.62f;
    drawPage(c, oldPv, dx - navDir * k * Wd, ca * (1 - k), false);
    drawPage(c, pv, dx + navDir * (1 - k) * Wd, ca * k, true);
  } else {
    drawPage(c, pv, dx, ca, true);
  }
}

// Print setup: three settings + the big pink PRINT button (selected by default)
static void drawSetup(Canvas& c, float dx, float ca, float dt) {
  suPill.target = suSel;
  suPill.step(dt, 420, 30);
  suBtn.target = suSel == 3 ? 1 : 0;
  suBtn.step(dt, 10);
  float e = clampf(edit.v, 0, 1.15f);
  float la = ca * clampf(1 - e * 1.6f, 0, 1);
  FileEntry& f = files[suFile];
  // the rows on the left, the button on the right
  float nameY = 46, planY = 62, rowY0 = 84, rowH = 26, rowX = 8, rowW = L.W - 124.f, pillH = 24;
  float bx = L.W - 106.f + dx, by = 74, bw = 98, bh = 74;
  int wy = L.H - 8, pad = 12;
  const Font* fName = &F_MD;
  const Font* fVal = &F_MD;
  int tx = (int)(12 + dx), tw = L.W - 24;
  // a warning if something will make the dose wrong
  float dev = f.planRevs > 0 ? 100.f * (suTurns / (f.planRevs * (1 + suFactor / 100.f)) - 1.f) : 0;
  float tol = f.tol > 0 ? f.tol : 5.f;
  char warn[64] = "", nm[64], b[64], b2[64];
  if (!f.cadOk) snprintf(warn, sizeof warn, "%s", T(S_W_CADENCE));
  else if (f.planRevs > 0 && fabsf(dev) > tol / 2)
    snprintf(warn, sizeof warn, "%s %+.0f%% · %s ±%.0f%%", T(S_W_DOSE), (double)dev, T(S_PLAN), (double)tol);
  fitText(*fName, f.name, nm, sizeof nm, tw);
  c.text(*fName, tx, (int)nameY, nm, LIGHT, ca);
  uint16_t planC = DIM;
  if (f.planRevs > 0) {
    if (f.tol > 0) snprintf(b, sizeof b, "%s · %s %.2f %s · ±%.0f%%", f.usb ? T(S_USB) : "SpinPi", T(S_PLAN), (double)f.planRevs, T(S_TURNS_UNIT), (double)f.tol);
    else snprintf(b, sizeof b, "%s · %s %.2f %s", f.usb ? T(S_USB) : "SpinPi", T(S_PLAN), (double)f.planRevs, T(S_TURNS_UNIT));
  } else {
    snprintf(b, sizeof b, "%s", T(S_NO_PLAN));
    planC = PINK;
  }
  fitText(F_SM, b, b2, sizeof b2, tw);
  c.text(F_SM, tx, (int)planY, b2, planC, ca);
  // rows
  const ItemId rows[3] = {I_SU_TURNS, I_SU_FACTOR, I_SU_DELAY};
  float pillY = rowY0 + suPill.v * rowH;
  float rowsA = la * (1 - clampf(suPill.v - 2, 0, 1));
  if (suPill.v < 2.99f) c.fillRoundRect(rowX + dx, pillY - pillH / 2, rowW, pillH, pillH / 2, ACT, rowsA);
  for (int i = 0; i < 3; i++) {
    float ry = rowY0 + i * rowH;
    float k = clampf(1 - fabsf(ry - pillY) / rowH, 0, 1) * (suPill.v < 2.99f ? 1 : 0);
    c.text(F_SM, (int)(rowX + pad + dx + k * 2), (int)ry + 5, T(ITEM_NAME[rows[i]]), lerpColor(ROWTXT, DARK, k), la);
    char v[24];
    if (i == 0) { fmtTime(v, sizeof v, (int)lroundf(suTurns * f.revS)); snprintf(b, sizeof b, "%d · %s", suTurns, v); }
    else if (i == 1) snprintf(b, sizeof b, "%+.0f%%", (double)suFactor);
    else snprintf(b, sizeof b, "%.0f %s", (double)suDelay, T(S_SEC));
    c.textRight(*fVal, (int)(rowX + rowW - 10 + dx), (int)ry + 6, b, lerpColor(LIGHT, DARK, k), la);
  }
  // the button
  float s = suBtn.v;
  float pulse = s * (0.5f + 0.5f * sinf(now * 4));
  float grow = 3 * s, br = 18;
  if (s > 0.02f) c.glow(bx + bw / 2, by + bh / 2, 70, HOT, 0.28f * s * la + 0.08f * pulse * la);
  c.fillRoundRect(bx - grow, by - grow, bw + grow * 2, bh + grow * 2, br, lerpColor(BTN, ACT, 0.35f + 0.65f * s), la);
  if (s > 0.02f) c.strokeRoundRect(bx - grow - 3, by - grow - 3, bw + grow * 2 + 6, bh + grow * 2 + 6, br + 3, 1.5f, LIGHT, s * la * (0.5f + 0.5f * pulse));
  c.textCentered(F_MD, (int)(bx + bw / 2), (int)(by + bh / 2 + 5), T(S_PRINT_BTN), DARK, la);
  c.textCentered(F_SM, (int)(bx + bw / 2), (int)(by + bh / 2 + 24), "OK", DARK, la * 0.55f * s);
  // the warning on a line of its own, else the hint
  if (warn[0]) { fitText(F_SM, warn, b2, sizeof b2, L.W - 24); c.text(F_SM, tx, wy, b2, PINK, la); }
  else c.text(F_SM, tx, wy, HT(S_SETUP_HINT), FAINT, la * 0.8f);
  float x, y, w, h;
  editTarget(x, y, w, h);
  x = 8; w = rowW;
  drawEditCard(c, rowX + dx, pillY - PILL_H / 2.f, rowW, x + dx, y, w, h, ca);
}

// the silhouette centred on (cx, cy): one rectangle per run of set pixels in a row (cheap)
static void drawShape(Canvas& c, float cx, float cy, uint16_t col, float a) {
  if (!shapeOk || a < 0.01f) return;
  int bpr = (shapeW + 7) / 8;
  float x0 = cx - shapeW / 2.f, y0 = cy - shapeH / 2.f;
  for (int y = 0; y < shapeH; y++) {
    const uint8_t* row = shapeBits + y * bpr;
    int x = 0;
    while (x < shapeW) {
      while (x < shapeW && !(row[x >> 3] & (0x80 >> (x & 7)))) x++;
      int s = x;
      while (x < shapeW && (row[x >> 3] & (0x80 >> (x & 7)))) x++;
      if (x > s) c.fillRect(x0 + s, y0 + y, x - s, 1, col, a);
    }
  }
}

// the main page's last line: is the printer ready, and if not, the first thing that is missing
static void drawReady(Canvas& c, float dx, float a) {
  if (a < 0.01f) return;
  char b[64], fb[64];
  uint16_t col = PINK;
  bool pi = piOk();
  if (!pi) { snprintf(b, sizeof b, "%s", T(S_NO_RASP)); col = DIM; }
  else if (oldSpinPi()) snprintf(b, sizeof b, "%s", T(S_OLD_SPINPI));
  else if (sysHdmi == 0) { snprintf(b, sizeof b, "%s", T(S_NO_PROJ)); col = DIM; }
  else if (updState == UPD_AVAIL) snprintf(b, sizeof b, T(S_UPD_HAVE), updVer);
  else snprintf(b, sizeof b, "%s", T(S_READY));
  bool good = pi && !oldSpinPi() && sysHdmi != 0;
  int x = (int)(LIST_X + ROW_PAD + dx), r = (int)(LIST_X + LIST_W - ROW_PAD + dx), y = L.H - 12;
  float pulse = good ? 1 : 0.55f + 0.45f * sinf(now * 3);
  c.fillCircle(x + 3, y - 4, 3, good ? PINK : (pi ? HOT : FAINT), a * pulse);
  // where there is room: the two links as dots on the right
  int dotsW = 0;
  if (LIST_W > 250) {
    const char* names[2] = {T(S_PI), T(S_PROJECTOR)};
    bool on[2] = {pi, pi && sysHdmi == 1};
    int rx = r;
    for (int i = 1; i >= 0; i--) {
      int tw = Canvas::textWidth(F_SM, names[i]);
      c.text(F_SM, rx - tw, y, names[i], on[i] ? DIM : FAINT, a);
      c.fillCircle(rx - tw - 7, y - 4, 2.5f, on[i] ? PINK : FAINT, a);
      rx -= tw + 18;
    }
    dotsW = r - rx;
  }
  fitText(F_SM, b, fb, sizeof fb, r - x - 12 - dotsW);
  c.text(F_SM, x + 12, y, fb, col, a);
}

static void drawPrint(Canvas& c, float dx, float ca) {
  float cx = L.artCx, cy = 100, R = 52;
  float p = clampf(prog.v, 0, 1);
  float th = 4;
  c.ring(cx, cy, R, th, PANEL, ca);
  if (infinite) {
    float a = now * 1.6f;
    c.arc(cx, cy, R, th, a, a + 1.2f, PINK, ca);
  } else if (p > 0.002f) {
    c.arc(cx, cy, R, th, -1.5708f, -1.5708f + 6.2832f * p, PINK, ca);
  }
  char b[48];
  if (infinite) snprintf(b, sizeof b, "∞");
  else snprintf(b, sizeof b, "%d%%", (int)(p * 100 + 0.5f));
  if (shapeOk) {
    // the part inside the vial: dark at first, lighting up with the dose. The light is on from the end
    // of the wait; the dot on the inner circle still shows the vial turning.
    float ri = R - 10, ang = (float)angleNow() * 0.0174533f - 1.5708f;
    c.ring(cx, cy, ri, 1.2f, LINE, ca);
    c.fillCircle(cx + cosf(ang) * ri, cy + sinf(ang) * ri, 2.4f, PINK, ca);
    drawShape(c, cx, cy, FAINT, ca);
    bool lit = waitKind[0] == 0;
    float k = infinite ? 0.6f : p;
    float glow = lit ? (0.25f + 0.75f * sqrtf(k)) * (0.88f + 0.12f * sinf(now * 5)) : 0;
    if (glow > 0.02f) c.glow(cx, cy, R * 0.8f, HOT, 0.10f * glow * ca);
    drawShape(c, cx, cy, PINK, ca * glow);
    c.textCentered(F_MD, (int)cx, (int)(cy + R + 24), b, LIGHT, ca);
  } else {
    drawDial(c, cx, cy, R - 12, ca, false);
    c.textCentered(F_MD, (int)cx, (int)cy + 6, b, LIGHT, ca);
  }

  // text column: right of the dial
  int x = (int)(132 + dx), r = L.W - 8;
  int yName = 46, yBig = 92, yLab = 110, yRpm = 134, yHint = L.H - 8;
  const Font& big = F_XL;
  char nm[64];
  fitText(F_SM, fileName[0] ? fileName : T(S_PRINTING), nm, sizeof nm, r - x);
  c.text(F_SM, x, yName, nm, DIM, ca);
  char t[24];
  bool waiting = waitKind[0] != 0;
  if (waiting) {  // light still off: spin-up, warm-up or dark pause
    const char* what = !strcmp(waitKind, "warm") ? T(S_WAIT_WARM) : (!strcmp(waitKind, "dark") ? T(S_WAIT_DARK) : T(S_WAIT_SPIN));
    fmtTime(t, sizeof t, waitSec);
    c.text(big, x - 2, yBig, t, LIGHT, ca * (0.6f + 0.4f * sinf(now * 3)));
    fitText(F_SM, what, nm, sizeof nm, r - x);
    c.text(F_SM, x, yLab, nm, PINK, ca);
  } else {
    fmtTime(t, sizeof t, secLeft);
    c.text(big, x - 2, yBig, t, PINK, ca);
    c.text(F_SM, x, yLab, infinite ? T(S_ELAPSED) : T(S_LEFT), DIM, ca);
  }
  if (syncMs >= 0 && !waiting) {
    snprintf(b, sizeof b, "%s ±%.0f ms", T(S_SYNC), (double)syncMs);
    c.textRight(F_SM, r, yLab, b, syncMs < 20 ? PINK : DIM, ca);
  }
  char rr[16];
  fmt2(rr, sizeof rr, H.motorRpm());
  snprintf(b, sizeof b, "%s %s", rr, T(S_RPM));
  c.text(F_SM, x, yRpm, b, LIGHT, ca);
  if (turnsTotal > 0 && !waiting) {
    int rev = (int)(p * turnsTotal) + 1;
    if (rev > turnsTotal) rev = turnsTotal;
    snprintf(b, sizeof b, "%s %d %s %d", T(S_REV), rev, T(S_OF), turnsTotal);
    c.textRight(F_SM, r, yRpm, b, DIM, ca);
  }
  if (drops > 0 && !stopping) {
    snprintf(b, sizeof b, "%s: %d", T(S_DROPS), drops);
    c.text(F_SM, x, yHint, b, PINK, ca);
  } else {
    const char* hint = stopping ? T(S_STOPPING) : (S.dark ? T(S_DARKENED) : HT(S_HOLD_STOP));
    fitText(F_SM, hint, nm, sizeof nm, r - x);
    c.text(F_SM, x, yHint, nm, stopping ? PINK : FAINT, ca);
  }
}

static void drawDone(Canvas& c, float dx, float ca, float dt) {
  for (auto& p : conf) {
    p.vy += 160 * dt;
    p.vx *= 1 - dt * 0.8f;
    p.x += p.vx * dt + sinf(now * 4 + p.r * 9) * 0.3f;
    p.y += p.vy * dt;
    if (p.y < L.H + 5) c.fillCircle(p.x, p.y, p.r, p.c, 0.9f);
  }
  static const Str OPT[3] = {S_R_UNDER, S_R_OK, S_R_OVER};
  int x = (int)(LIST_X + 20 + dx);
  c.text(F_LG, x, 60, T(S_DONE), PINK, ca);
  c.text(F_SM, x, 82, T(S_HOW), LIGHT, ca * 0.85f);
  float bx = LIST_X + 12 + dx, pw = L.W - 8 - (LIST_X + 12), ph = 28, by = 98;
  for (int i = 0; i < 3; i++) {  // the answer moves the exposure correction for the next print
    float y = by + i * (ph + 6);
    bool on = i == doneSel;
    c.fillRoundRect(bx, y, pw, ph, ph / 2, on ? ACT : PANEL, ca);
    c.text(F_SM, (int)(bx + 14), (int)y + 19, T(OPT[i]), on ? DARK : LIGHT, ca);
    if (on) c.textRight(F_SM, (int)(bx + pw - 14), (int)y + 19, "→", DARK, ca);
  }
  char h[64];
  fitText(F_SM, HT(S_DONE_HINT), h, sizeof h, L.W - x - 4);
  c.text(F_SM, x, L.H - 8, h, FAINT, ca * 0.8f);
}

// A print sent from SpinSlicer: what it is, and the big pink yes (→) or no (←)
static void drawAsk(Canvas& c, float dx, float ca) {
  int x = (int)(LIST_X + 6 + dx), r = L.W - 8;
  char b[64], t[24];
  c.text(F_SM, x, 46, T(S_ASK_FROM), DIM, ca);
  fitText(F_MD, askName, b, sizeof b, r - x);
  c.text(F_MD, x, 70, b, LIGHT, ca);
  fmtTime(t, sizeof t, askSec);
  if (askTurns > 0) snprintf(b, sizeof b, "%d %s · %s", askTurns, T(S_TURNS_UNIT), t);
  else snprintf(b, sizeof b, "%s", t);
  c.text(F_SM, x, 92, b, LIGHT, ca * 0.85f);
  float pulse = askSent ? 0 : 0.5f + 0.5f * sinf(now * 4);
  float bx = x, by = 112, bw = r - x, bh = 70;
  c.glow(bx + bw / 2, by + bh / 2, 70, HOT, 0.28f * ca + 0.08f * pulse * ca);
  c.fillRoundRect(bx, by, bw, bh, 18, ACT, ca);
  c.strokeRoundRect(bx - 3, by - 3, bw + 6, bh + 6, 21, 1.5f, LIGHT, ca * (0.5f + 0.5f * pulse));
  c.textCentered(F_LG, (int)(bx + bw / 2), (int)(by + bh / 2 + 8), T(askSent ? S_ASK_STARTING : S_ASK_TITLE), DARK, ca);
  if (!askSent) c.text(F_SM, x, L.H - 8, T(S_SURE_HINT), FAINT, ca * 0.8f);
}

static void drawConnect(Canvas& c, float dx, float ca) {
  char b[64], fb[64];
  if (!qrOk) {
    int x = (int)((LIST_X + 6) + dx), y = 74;
    c.text(F_MD, x, y, T(S_NOPI1), LIGHT, ca);
    fitText(F_SM, T(S_NOPI2), fb, sizeof fb, L.W - x - 4);
    c.text(F_SM, x, y + 22, fb, DIM, ca);
    fitText(F_SM, HT(S_BACK_HINT), fb, sizeof fb, L.W - x - 4);
    c.text(F_SM, x, L.H - 8, fb, FAINT, ca * 0.7f);
    return;
  }
  int n = qrcodegen_getSize(qr);
  int box = 104;
  int sc = box / (n + 4);
  if (sc < 1) sc = 1;
  int sz = (n + 4) * sc;
  float qcx = 62;
  float qcy = 100;
  float qx = qcx - sz / 2.f, qy = qcy - sz / 2.f;
  float pop = easeOut((now - enterT) / 0.45f);
  c.fillRoundRect(qx - 4, qy - 4, sz + 8, sz + 8, 10, WHITE, ca * pop);
  for (int yy = 0; yy < n; yy++)
    for (int xx = 0; xx < n; xx++)
      if (qrcodegen_getModule(qr, xx, yy))
        c.fillRect((int)qx + (xx + 2) * sc, (int)qy + (yy + 2) * sc, sc, sc, INK, ca * pop);
  int x = (int)(LIST_X + 6 + dx);
  c.text(F_MD, x, 52, T(S_OPEN_PHONE), LIGHT, ca);
  c.text(F_SM, x, 76, url, PINK, ca);
  if (host[0]) { snprintf(b, sizeof b, "%s %s", T(S_OR), host); c.text(F_SM, x, 96, b, DIM, ca); }
  if (wifi[0]) { snprintf(b, sizeof b, "%s: %s", T(S_WIFI), wifi); c.text(F_SM, x, 122, b, DIM, ca); }
  c.text(F_SM, x, L.H - 8, HT(S_BACK_HINT), FAINT, ca * 0.7f);
}

static void drawConfirm(Canvas& c) {
  float a = clampf(confirm.v, 0, 1);
  if (a < 0.01f) return;
  c.fillRect(0, 0, L.W, L.H, BG, 0.75f * a);
  float w = fminf(230.f, L.W - 16.f), h = 92, x = L.W / 2.f - w / 2, y = L.H / 2.f - h / 2 + (1 - a) * 14;
  c.fillRoundRect(x, y, w, h, 16, PANEL, a);
  c.strokeRoundRect(x, y, w, h, 16, 1.2f, HOT, a);
  char ct[48];
  if (confirmItem == I_UPD) snprintf(ct, sizeof ct, "%s %s", T(S_UPD_DO), updVer);
  else snprintf(ct, sizeof ct, "%s", T(ITEM_NAME[confirmItem]));
  c.textCentered(F_SM, L.W / 2, (int)y + 26, ct, DIM, a);
  c.textCentered(F_LG, L.W / 2, (int)y + 56, T(S_SURE), PINK, a);
  char hb[48];
  fitText(F_SM, HT(S_SURE_HINT), hb, sizeof hb, (int)w - 16);
  c.textCentered(F_SM, L.W / 2, (int)y + 78, hb, LIGHT, a * 0.7f);
}

// the first-setup wizard: one step a page. A progress bar on top, the step's text, the value it
// sets (big), a checklist where it helps, the dial on the motor steps.
static void wizRow(Canvas& c, int x, int r, int y, bool ok, const char* name, const char* val, float a) {
  c.text(F_SM, x, y, ok ? "✓" : "•", ok ? PINK : FAINT, a);
  c.text(F_SM, x + 16, y, name, DIM, a);
  char v[64];
  fitText(F_SM, val, v, sizeof v, r - x - 16 - Canvas::textWidth(F_SM, name) - 8);
  c.textRight(F_SM, r, y, v, ok ? LIGHT : DIM, a);
}
static void drawWiz(Canvas& c, float dx, float ca) {
  bool dial = wizMotorStep(wizStep);
  int x = (int)((dial ? 132 : 16) + dx), r = L.W - 12, w = r - x;
  float a = ca * easeOut((now - wizT) / 0.35f);
  char b[64], fb[64];
  // header + progress
  snprintf(b, sizeof b, "%s", T(S_WIZ_STEP));
  c.text(F_SM, x, 44, b, DIM, ca);
  snprintf(b, sizeof b, "%d / %d", wizStep + 1, (int)WZ_COUNT);
  c.textRight(F_SM, r, 44, b, DIM, ca);
  static Ease bar;
  bar.target = (wizStep + 1) / (float)WZ_COUNT;
  bar.step(1 / 60.f, 10);
  c.fillRoundRect(x, 50, w, 4, 2, LINE, ca);
  c.fillRoundRect(x, 50, fmaxf(4, w * bar.v), 4, 2, PINK, ca);

  static const Str TITLE[WZ_COUNT] = {S_WIZ_HELLO, S_WIZ_LINK, S_WIZ_FOCUS, S_P_CENTER, S_P_VIAL,
                                      S_WIZ_DIR,   S_ONEREV,   S_SPEED,     S_BRIGHT,   S_DONE};
  static const Str LINE1[WZ_COUNT] = {S_WIZ_HELLO1, S_COUNT,     S_WIZ_FOCUS1, S_WIZ_CENTER1, S_WIZ_VIAL1,
                                      S_WIZ_DIR1,   S_WIZ_TURN1, S_WIZ_SPEED1, S_WIZ_SCREEN1, S_WIZ_DONE1};
  static const Str LINE2[WZ_COUNT] = {S_COUNT,    S_COUNT,     S_COUNT,      S_COUNT, S_WIZ_VIAL2,
                                      S_WIZ_DIR2, S_WIZ_TURN2, S_WIZ_SPEED2, S_COUNT, S_COUNT};
  const char* title = T(TITLE[wizStep]);
  const Font& tf = Canvas::textWidth(F_LG, title) <= w ? F_LG : F_MD;
  fitText(tf, title, fb, sizeof fb, w);
  c.text(tf, x, (int)(80 + (1 - a) * 6), fb, wizStep == WZ_DONE ? PINK : LIGHT, a);
  bool pi = piOk();
  bool noPi = wizNeedsPi(wizStep) && !pi;
  Str l1 = noPi ? S_WIZ_NEED_PI : LINE1[wizStep];
  if (wizStep == WZ_LINK) l1 = pi ? S_WIZ_LINK_OK : S_WIZ_LINK_NO;
  char para[160];
  if (!noPi && LINE2[wizStep] != S_COUNT) snprintf(para, sizeof para, "%s %s", T(l1), T(LINE2[wizStep]));
  else snprintf(para, sizeof para, "%s", T(l1));
  int textEnd = wrapText(c, F_SM, x, 102, w, 17, para, noPi || (wizStep == WZ_LINK && !pi) ? PINK : DIM, a,
                         dial ? 4 : 2);
  int vy = textEnd > 136 ? textEnd + 30 : 166;  // the big value goes below the text

  // the value this step sets, big; or a checklist
  float bx = bump.v > 0 ? bumpDir * bump.v * -4 : 0;
  switch (wizStep) {
    case WZ_HELLO: c.text(F_LG, x, 160, T(S_LANGNAME), PINK, a); break;
    case WZ_LINK: {
      wizRow(c, x, r, textEnd + 12, pi, T(S_PI), pi ? T(S_CONNECTED) : T(S_NOT_CONNECTED), a);
      wizRow(c, x, r, textEnd + 32, sysHdmi == 1, T(S_WIZ_HDMI), sysHdmi == 1 ? T(S_CONNECTED) : (sysHdmi == 0 ? T(S_NOT_CONNECTED_M) : "—"), a);
      bool net = pi && sysIp[0];
      wizRow(c, x, r, textEnd + 52, net, T(S_NET), net ? sysIp : "—", a);
      break;
    }
    case WZ_FOCUS: {
      static const Str PN[3] = {S_P_FOCUS, S_P_GRID, S_P_CIRCLE};
      if (!noPi) c.text(F_LG, (int)(x + bx), 160, T(PN[wizPat]), PINK, a);
      break;
    }
    case WZ_CENTER: case WZ_VIAL:
      if (noPi) break;
      snprintf(b, sizeof b, wizStep == WZ_CENTER ? "%+d" : "%d", wizStep == WZ_CENTER ? calOffset : calVial);
      c.text(F_XL, (int)(x + bx), 180, b, PINK, a);
      c.text(F_SM, x + Canvas::textWidth(F_XL, b) + 6, 180, T(S_PX), DIM, a);
      break;
    case WZ_DIR: c.text(F_LG, (int)(x + bx), vy, T(S.dirInv ? S_CCW : S_CW), PINK, a); break;
    case WZ_TURN: {
      float done = clampf(fmodf((float)angleNow() + 3600, 360.f) / 360.f, 0, 1);
      if (H.motorRpm() < 0.01f && angleNow() > 359) done = 1;
      snprintf(b, sizeof b, "%.0f°", (double)fminf(360.f, (float)angleNow()));
      c.text(F_LG, x, vy, b, PINK, a);
      c.fillRoundRect(x, vy + 10, w, 4, 2, LINE, a);
      c.fillRoundRect(x, vy + 10, fmaxf(4, w * done), 4, 2, PINK, a);
      break;
    }
    case WZ_SPEED: {
      char rr[16];
      fmt2(rr, sizeof rr, S.rpm);
      c.text(F_XL, (int)(x + bx), vy + 18, rr, PINK, a);
      c.text(F_SM, x + Canvas::textWidth(F_XL, rr) + 6, vy + 18, T(S_RPM), DIM, a);
      break;
    }
    case WZ_SCREEN: {
      snprintf(b, sizeof b, "%d%%", S.bright);
      c.text(F_XL, (int)(x + bx), 180, b, PINK, a);
      break;
    }
    case WZ_DONE: {  // what the printer is now
      char v[64], rr[16];
      wizRow(c, x, r, textEnd + 6, pi, T(S_PI), pi ? T(S_CONNECTED) : T(S_NOT_CONNECTED), a);
      wizRow(c, x, r, textEnd + 24, sysHdmi == 1, T(S_PROJECTOR), sysHdmi == 1 ? T(S_CONNECTED) : "—", a);
      snprintf(v, sizeof v, "%+d %s · %d %s", calOffset, T(S_PX), calVial, T(S_PX));
      wizRow(c, x, r, textEnd + 42, pi, T(S_P_CENTER), v, a);
      fmt2(rr, sizeof rr, S.rpm);
      snprintf(v, sizeof v, "%s · %s %s", T(S.dirInv ? S_CCW : S_CW), rr, T(S_RPM));
      wizRow(c, x, r, textEnd + 60, true, T(S_MOTOR), v, a);
      break;
    }
  }
  Str hint = S_WIZ_HINT_NEXT;
  if (wizStep == WZ_HELLO) hint = S_WIZ_HINT0;
  else if (wizStep == WZ_FOCUS && !noPi) hint = S_WIZ_HINT_PIC;
  else if ((wizStep == WZ_CENTER || wizStep == WZ_VIAL) && !noPi) hint = S_WIZ_HINT;
  else if (wizStep == WZ_SPEED || wizStep == WZ_SCREEN) hint = S_WIZ_HINT;
  else if (wizStep == WZ_DIR) hint = S_WIZ_HINT_DIR;
  else if (wizStep == WZ_TURN) hint = S_WIZ_HINT_TURN;
  else if (wizStep == WZ_DONE) hint = S_WIZ_HINT_END;
  int hx = (int)(16 + dx);  // the dial ends above the hint line: it can take the whole width
  fitText(F_SM, HT(hint), fb, sizeof fb, r - hx);
  c.text(F_SM, hx, L.H - 10, fb, FAINT, ca * 0.8f);
  if (dial) drawDial(c, L.artCx, L.artCy, L.dialR, ca);
}

// while the Raspberry writes a new firmware the board stops answering: say so, big
static void drawFlashing(Canvas& c) {
  if (!fwFlashing && !updRun) return;
  c.fillRect(0, 0, L.W, L.H, BG, 0.9f);
  float pulse = 0.5f + 0.5f * sinf(now * 4);
  c.glow(L.W / 2.f, L.H / 2.f - 10, 80, HOT, 0.15f + 0.1f * pulse);
  char b[64];
  fitText(F_LG, T(updRun ? S_UPD_PI : S_FW_FLASHING), b, sizeof b, L.W - 16);
  c.textCentered(F_LG, L.W / 2, L.H / 2 - 4, b, PINK);
  fitText(F_SM, T(S_FW_DONT), b, sizeof b, L.W - 16);
  c.textCentered(F_SM, L.W / 2, L.H / 2 + 22, b, LIGHT);
  const char* sub = updRun ? T(S_UPD_PI_SUB) : (fwVer[0] ? fwVer : (updVer[0] ? updVer : ""));
  fitText(F_SM, sub, b, sizeof b, L.W - 16);
  c.textCentered(F_SM, L.W / 2, L.H / 2 + 42, b, DIM);
}

// ================================================================ Spinny's behaviour
static void think(float dt, bool visible, float s) {
  // where his head may go: inside his corner, a margin from the edges
  float mx = 26 * s, xMin = L.artX0 + mx, xMax = L.artX1 - mx;
  // his body trails behind the head, so where the head may go depends on which way he faces:
  // facing right the head stays at least a body length from the left edge, and the other way round
  float bodyL = 108 * s * 0.95f;
  float rMin = fminf(L.artX0 + bodyL, xMax), lMax = fmaxf(L.artX1 - bodyL, xMin);
  float yMin = L.artY0 + 30 * s, yMax = L.artY1 - 24 * s;
  bool idle = now - lastInput > 12, sleepy = now - lastInput > 60 && screen == SC_MENU && !motorOn;
  SpinnyGoal g;
  g.maxV = 42 * s;
  float headX, headY;
  spinnyHeadXY(B.body, headX, headY);
  float homeY = 84;
  if (!B.body.init) spinnyPlace(B.body, -20, homeY, s, true);  // enters from the left

  if (screen == SC_BOOT) {
    g.tx = L.artX1 - 24;  // across the logo, ends near his corner
    g.ty = homeY;
    g.maxV = 70 * s;
  } else if (screen == SC_DONE) {  // happy laps: dash across, flip round, dash back, then rest
    float u = now - enterT;
    float midY = 82;
    if (u < 5) {
      int lap = (int)(u / 1.25f);
      g.tx = lap & 1 ? xMin : xMax;
      g.ty = midY + ((lap & 1) ? -8 : 8) * s;
      g.maxV = 95 * s;
    } else { g.tx = (rMin + xMax) / 2; g.ty = midY; g.hover = true; }
  } else if (now - B.startleT < 0.7f) {  // frightened: dash away
    g.tx = B.startleDir > 0 ? xMax : xMin;
    g.ty = headY;
    g.maxV = 130 * s;
  } else if (sleepy) {  // stops, sinks to the bottom level and dozes there
    if (B.sleepX < 0) B.sleepX = headX;
    g.tx = B.sleepX;
    g.ty = yMax + 6 * s;
    g.hover = true;
    g.sink = 8 * s;
  } else if (B.food) {  // goes for the pellet, head first
    g.tx = B.fx;
    g.ty = B.fy;
    g.maxV = 65 * s;
    float ddx = headX - B.fx, ddy = headY - B.fy;
    if (ddx * ddx + ddy * ddy < 36 && visible) {  // gulp
      B.food = false;
      B.eatT = now;
      mood(MOOD_HAPPY, 1.2f);
      hop(3);
#ifdef SIM_LOG
      fprintf(stderr, "gulp at %.1f s\n", (double)now);
#endif
    }
  } else if (now - B.airT < 2.2f) {  // up for a breath of air
    float u = now - B.airT;
    g.tx = B.tx;
    g.ty = u < 1.1f ? L.artY0 + 10 * s : yMin + 18 * s;
    g.maxV = 40 * s;
    if (u > 1.0f && u < 1.4f) B.eatT = now - 0.2f;  // mouth open at the top
  } else {
    if (now > B.nextWander) {  // now and then: a few strokes on the same side, or turn and cross over
      bool right = B.side > 0;
      if (frand(0, 1) < 0.45f) B.tx = right ? frand(rMin, xMax) : frand(xMin, lMax);
      else B.tx = right ? frand(xMin, lMax) : frand(rMin, xMax);
      B.nextWander = now + frand(4.5f, 10);
    }
    float base = (yMin + yMax) / 2;
    if (screen == SC_MENU) {  // swim alongside the highlighted row
      float pillY = LIST_TOP + pv.pill.v - pv.scroll.v;
      base = base * 0.35f + clampf(pillY, yMin, yMax) * 0.65f;
    }
    g.tx = B.tx;
    g.ty = base + sinf(now * 0.45f) * 5 * s + B.kickY;
    // close to the spot: hover and just breathe
    float ddx = headX - g.tx, ddy = headY - g.ty;
    g.hover = ddx * ddx + ddy * ddy < 25;
    // treats and air, only when he is visible and left alone
    if (visible && idle && screen == SC_MENU && now > B.nextFood) {
#ifdef SIM_LOG
      fprintf(stderr, "food at %.1f s\n", (double)now);
#endif
      B.food = true;
      B.fx = frand(0, 1) < 0.5f ? frand(xMin + 6, lMax) : frand(rMin, xMax - 6);  // where he can reach it
      B.fy = L.artY0 + 4;
      B.fa = 0;
      B.nextFood = now + frand(25, 45);
    } else if (visible && idle && screen == SC_MENU && now > B.nextAir) {
      B.airT = now;
      B.nextAir = now + frand(35, 70);
    }
  }
  if (!sleepy) B.sleepX = -1;
  B.kickY *= expf(-dt * 2.5f);
  if (B.food) {
    B.fy += 9 * dt;
    B.fx += sinf(now * 2.3f) * 3 * dt;
    B.fa = fminf(1, B.fa + dt * 3);
    if (B.fy > yMax + 14 || !visible || screen != SC_MENU) B.food = false;
  }
  if (screen != SC_BOOT) {
    // Facing right the head needs a body length of room on its left, and the other way round,
    // so each side has its own range for the head. A goal that suits both sides (or neither:
    // the middle of a narrow corner) keeps the side he has. Without this, a goal in the middle
    // made him turn back and forth for ever (the "twitching").
    float tR = clampf(g.tx, rMin, xMax), tL = clampf(g.tx, xMin, lMax);
    float dR = fabsf(tR - g.tx), dL = fabsf(tL - g.tx);
    if (B.side > 0 ? dL + 10 < dR : dR + 10 < dL) B.side = -B.side;
    g.tx = B.side > 0 ? tR : tL;
    g.ty = clampf(g.ty, yMin, yMax + 6 * s);
    // a small step backwards on his own side: paddle there, no U-turn
    bool facing = spinnyFacingRight(B.body) == (B.side > 0);
    if (facing && (g.tx - headX) * B.side < -2 && fabsf(g.tx - headX) < 40 * s) g.hover = true;
    g.face = B.side;
    g.wx0 = L.artX0 + 1;
    g.wx1 = L.artX1 - 1;
    g.wy0 = L.artY0;
    g.wy1 = L.artY1;
  }
  spinnyUpdate(B.body, g, dt, s);
  float e = (now - B.eatT) / 0.4f;
  B.body.mouth = e >= 0 && e < 1 ? sinf(e * 3.14159f) : 0;
  spinnyCenter(B.body, B.x, B.y);
}

// ================================================================ frame
int ui_frame(Canvas& c, uint32_t nowMs, float subMs) {
  if (c.W != L.W || c.H != L.H) layout(c.W, c.H);
  now = ((nowMs - bootMs) + subMs) / 1000.f;
  float dt = clampf(now - last, 0.001f, 0.07f);
  last = now;
  fps += (1.f / dt - fps) * 0.05f;

  pal::theme(S.theme && !(S.dark && screen == SC_PRINT));
  if (screen == SC_BOOT && now > 2.6f) afterBoot();
  if (screen == SC_PRINT && everRx && !piOk()) {  // the Pi vanished mid-print: stop the motor here
    motorOn = false;
    H.spin(false, 0);
    go(SC_MENU);
    say(T(S_PI_LOST), 5);
    mood(MOOD_SAD, 5);
  }
  if (toastUntil >= 0 && now > toastUntil) toast.target = 0;
  if (tempMoodUntil >= 0 && now > tempMoodUntil) tempMoodUntil = -1;
  if (pv.sel >= pageCount(pv)) pv.sel = pageCount(pv) - 1;

  // list geometry (animated between "beside Spinny" and "without him")
  bool dial = (screen == SC_MENU && pv.page == PG_MOTOR) || screen == SC_PRINT || (screen == SC_CONNECT && qrOk);
  leftDial.target = dial ? 1 : 0;
  leftDial.step(dt, 8);
  bool axoOn = S.home == HOME_AXO;
  bool artOn = S.home == HOME_CARD && screen == SC_MENU && pv.page == PG_MAIN;
  homeArt.target = artOn ? 1 : 0;
  homeArt.step(dt, artOn ? 10 : 45);  // leaves fast, so it never ghosts through the next page
  // the card lights up quickly (a few frames): its whole area changes while it fades, and on a
  // slow SPI bus every one of those frames is expensive
  cardSel.target = (pv.page == PG_MAIN && pv.sel == 0) ? 1 : 0;
  cardSel.step(dt, 28);
  wide.target = (!axoOn && !dial) ? 1 : 0;
  wide.step(dt, 9);
  float w = wide.v;
  LIST_X = L.lx[0] + (L.lx[1] - L.lx[0]) * w;
  LIST_W = L.lw[0] + (L.lw[1] - L.lw[0]) * w;
  LIST_TOP = L.ltop[0] + (L.ltop[1] - L.ltop[0]) * w;
  LIST_CLIP = L.lclip[0] + (L.lclip[1] - L.lclip[0]) * w;
  VISIBLE = w > 0.5f ? L.vis[1] : L.vis[0];

  float top = rowPos(pv, pv.sel);
  float edge = rowEdge(pv, pv.sel), selH = rowHt(pv, pv.sel);
  if (pv.page == PG_MAIN && S.home == HOME_CARD) edge = fmx(edge - ROW_H, 0);  // rows under the card
  {  // the list starts to move one row before the selection reaches the edge: no sudden jumps
    float view = VISIBLE * ROW_H, all = pageHt(pv);
    float maxS = all > view ? all - view : 0;
    float pad = (VISIBLE >= 3 && all > view) ? ROW_H : 0;
    float lo = edge + selH - view + pad, hi = edge - pad;
    if (pv.scroll.target < lo) pv.scroll.target = lo;
    if (pv.scroll.target > hi) pv.scroll.target = hi;
    pv.scroll.target = clampf(pv.scroll.target, 0, maxS);
  }
  pv.pill.target = top;
  pv.pill.step(dt, 300, 27);  // softer spring: a little overshoot, no snapping
  pv.scroll.step(dt, 10);
  nav.step(dt, 8);
  edit.step(dt, 260, 24);
  bump.step(dt, 500, 34);
  toast.step(dt, 300, 22);
  confirm.step(dt, 300, 24);
  squash.step(dt, 400, 16);
  prog.step(dt, 4);

  float bootS = 1.2f, doneS = 0.8f;
  spS.target = screen == SC_BOOT ? bootS : (screen == SC_DONE ? doneS : L.spinS);
  spS.step(dt, 90, 16);

  Mood m = MOOD_IDLE;
  if (tempMoodUntil >= 0) m = tempMood;
  else if (screen == SC_PRINT) m = MOOD_WORK;
  else if (screen == SC_DONE) m = MOOD_HAPPY;
  else if (screen == SC_MENU && !editing && !motorOn && now - lastInput > 60) m = MOOD_SLEEPY;

  spHide.target = screen == SC_SETUP || screen == SC_WIZ ? 1 : 0;

  // a check the Raspberry never answered (old SpinPi, no link); an update that never came back
  if (updState == UPD_CHECKING && now - updT > 40) { updState = UPD_ERR; say(T(S_UPD_NOANSWER), 4); }
  if (updRun && now - updRunT > 15 * 60) { updRun = false; updState = UPD_ERR; }
  // sleep: the backlight goes off after S.sleepMin idle minutes on a quiet screen. Anything that
  // needs the screen (a print, "Print?", a message, a key) wakes it.
  bool sleepy = S.sleepMin > 0 && (screen == SC_MENU || screen == SC_CONNECT) && !motorOn && !editing &&
                confirm.target == 0 && !fwFlashing && !updRun && now - lastInput > S.sleepMin * 60.f;
  if (sleepy != asleep) {
    asleep = sleepy;
    H.brightness(asleep ? 0 : (screen == SC_PRINT && S.dark ? 10 : S.bright));
  }
  spHide.step(dt, 14);
  static Ease axoFade;
  axoFade.target = S.home == HOME_AXO ? 1 : 0;
  axoFade.step(dt, 10);
  bootHide.target = 0;
  bootHide.step(dt, 5);
  if (screen == SC_BOOT) bootHide.v = 1;  // after the step, or he would show through a little
  float sa = clampf(1 - leftDial.v * 1.6f, 0, 1) * (1 - spHide.v) * axoFade.v * (1 - bootHide.v);
  float da = clampf(leftDial.v * 1.6f - 0.6f, 0, 1);
#ifdef UI_PROBE
  extern float ui_probe_sa;
  ui_probe_sa = sa;
#endif
  think(dt, sa > 0.5f, spS.v);

  c.noClip();
  c.clear(BG);
  float ce = easeOut((now - enterT) / 0.4f);
  float dx = (1 - ce) * 26, ca = ce;
  if (sa > 0.01f) c.glow(B.x, B.y, 60 * spS.v, HOT, 0.10f * sa);

  // Spinny first: menus, cards and text are drawn over him
  if (B.food && sa > 0.01f) {  // the pellet
    c.fillCircle(B.fx, B.fy, 2.2f, hex(0xE0B070), B.fa * sa);
    c.fillCircle(B.fx - 0.6f, B.fy - 0.7f, 0.8f, WHITE, 0.6f * B.fa * sa);
  }
  if (sa > 0.01f && !(screen == SC_CONNECT && qrOk))
    drawSpinny(c, B.body, now, m, spinnyPalette(S.axoColor, now), clampf(squash.v, -1, 1), sa);
  if (m == MOOD_SLEEPY && sa > 0.5f) {
    for (int i = 0; i < 3; i++) {
      float u = fmodf(now * 0.4f + i / 3.f, 1.f);
      float hx, hy;
      spinnyHeadXY(B.body, hx, hy);
      float zx = hx + (spinnyFacingRight(B.body) ? 4 : -14) * spS.v;
      c.text(F_SM, (int)(zx + u * 10), (int)(B.y - 22 - u * 26), "z", LIGHT, sinf(u * 3.14159f) * 0.7f);
    }
  }

  if (screen != SC_BOOT) drawTopBar(c);
  switch (screen) {
    case SC_BOOT: {
      float pop = easeOut(now / 0.6f), fade = 1 - clampf((now - 2.3f) / 0.3f, 0, 1);
      float sz = 84.f;
      float cy = (L.H - 34) / 2.f;
      int ty = L.H - 24, sy = L.H - 6;
      c.glow(L.W / 2.f, cy, sz * 0.9f, HOT, 0.10f * pop * fade);
      drawLogo(c, L.W / 2.f, cy, sz * (0.7f + 0.3f * pop), pop * fade, (easeOut(now / 1.8f) - 1) * 6.2832f);
      float a = easeOut((now - 0.9f) / 0.6f) * fade;
      char tl[64];
      fitText(F_SM, T(S_TAGLINE), tl, sizeof tl, L.W - 8);
      c.textCentered(*FBIG, L.W / 2, (int)(ty + (1 - a) * 8), "SpinSlicer", PINK, a);
      c.textCentered(F_SM, L.W / 2, (int)(sy + (1 - a) * 8), tl, DIM, a);
      break;
    }
    case SC_MENU:
      if (homeArt.v > 0.08f) drawHomeCard(c, homeArt.v * ca, cardSel.v);
      drawMenu(c, dx, ca);
      if (pv.page == PG_MAIN) drawReady(c, dx, ca * (navDir != 0 ? easeOut(nav.v) : 1));
      break;
    case SC_SETUP: drawSetup(c, dx, ca, dt); break;
    case SC_PRINT: drawPrint(c, dx, ca); break;
    case SC_DONE: drawDone(c, dx, ca, dt); break;
    case SC_CONNECT: drawConnect(c, dx, ca); break;
    case SC_ASK: drawAsk(c, dx, ca); break;
    case SC_WIZ: drawWiz(c, dx, ca); break;
  }
  // (on the stacked layouts the edit card covers the dial's band: the dial goes while it is open)
  if (screen == SC_MENU && da > 0.01f) drawDial(c, L.artCx, L.artCy, L.dialR, da);

  drawConfirm(c);
  drawFlashing(c);
  drawToast(c);

  // frame rate: a steady 60 whenever anything is alive on screen (changing it on the fly is what
  // looks like stutter). Only a fully still screen or a sleeping Spinny drops to 30.
  bool still = (sa < 0.01f || m == MOOD_SLEEPY) && da < 0.01f && H.motorRpm() < 0.01f && screen == SC_MENU &&
               now - lastInput > 3 && !editing && nav.v > 0.99f && toast.v < 0.01f && confirm.v < 0.01f;
  static float stillSince = -1;
  if (!still) stillSince = -1;
  else if (stillSince < 0) stillSince = now;
  return still && now - stillSince > 2 ? 30 : 60;
}

void ui_stats(float frameMs, float kbPerFrame, float drawMs, int cap) {
  if (cap > 0) capFps = cap;
  renderMs += (frameMs - renderMs) * 0.1f;
  if (drawMs >= 0) drawMsAvg += (drawMs - drawMsAvg) * 0.1f;
  pushKB += (kbPerFrame - pushKB) * 0.1f;
}

#ifdef UI_PROBE  // PC tests only (sim/spinny_stress.cpp): look at Spinny from outside
float ui_probe_sa = 0;
const SpinnyBody& ui_probe_body() { return B.body; }
void ui_probe_area(float* a) { a[0] = L.artX0; a[1] = L.artX1; a[2] = L.artY0; a[3] = L.artY1; a[4] = spS.v; a[5] = screen; }
#endif
