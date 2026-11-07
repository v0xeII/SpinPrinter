// Dirty test: runs the real UI with random key presses and checks that what the dirty-rectangle
// logic sends keeps a model of the panel identical to the framebuffer, frame after frame.
// Both paths: dirtyTiles (one framebuffer, tile hashes) and dirtyRects (two framebuffers).
//   ./dirty_test            all sizes, both main-page modes; exit code 1 on any mismatch
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "config.h"
#include "dirty.h"
#include "gfx.h"
#include "ui.h"

static float cur = 0, tgt = 0;
static double ang = 0;
static void hs(const char*) {}
static void hsp(bool o, float r) { tgt = o ? r : 0; }
static void hb(int) {}
static void hsv(const Settings&) {}
static float hr() { return cur; }
static double ha() { return ang; }
static void hc(const Settings&) {}
static void hz() { ang = 0; }
static void hm(double, float) {}

static const char* INPUT_NAME[4] = {"", "buttons", "encoder", "joystick"};
static int run(int W, int H, bool round, int home, bool tiles, unsigned seed, int input = 1) {
  ui_set_panel(round ? "GC9A01" : "", round);
  ui_set_input(input);
  std::vector<uint16_t> fb(W * H), prev(W * H, 0), panel(W * H, 0);
  std::vector<uint32_t> th(dirtyTileCount(W, H), 0);
  Canvas c(W, H, fb.data());
  UiHal hal{hs, hsp, hb, hsv, hr, ha, hc, hz, hm};
  Settings s{4.0f, 0, 80, 16, 200, 6.f, 0, 1.0f, 1, home, 0};
  ui_init(hal, s, 0);
  srand(seed);
  long bad = 0, frames = 0, rectsTotal = 0;
  bool first = true;
  for (int ms = 0; ms < 120000; ms += 16) {
    if (ms % 1000 == 0) ui_line("PING", ms);
    if (ms > 3000 && rand() % 9 == 0) {
      static const Key ks[] = {K_UP, K_DOWN, K_DOWN, K_UP, K_OK, K_BACK, K_BACK};
      ui_key(ks[rand() % 7], ms);
    }
    if (ms % 20000 == 10000) hsp(true, 8);
    if (ms % 20000 == 15000) hsp(false, 0);
    cur += (tgt - cur) * 0.03f;
    ang += cur * 6 * 0.016;
    ui_frame(c, ms);
    DirtyRect r[24];
    int n;
    if (first) { r[0] = {0, 0, W, H}; n = 1; if (tiles) dirtyTiles(fb.data(), W, H, th.data(), r + 1, 1); first = false; }
    else n = tiles ? dirtyTiles(fb.data(), W, H, th.data(), r, 24) : dirtyRects(fb.data(), prev.data(), W, H, r, 24);
    for (int i = 0; i < n; i++) {
      if (r[i].x < 0 || r[i].y < 0 || r[i].w <= 0 || r[i].h <= 0 || r[i].x + r[i].w > W || r[i].y + r[i].h > H) {
        printf("  bad rect %d,%d %dx%d at %d ms\n", r[i].x, r[i].y, r[i].w, r[i].h, ms);
        bad++;
        continue;
      }
      for (int y = r[i].y; y < r[i].y + r[i].h; y++)
        memcpy(&panel[y * W + r[i].x], &fb[y * W + r[i].x], r[i].w * 2);
    }
    rectsTotal += n;
    long diff = 0;
    for (int i = 0; i < W * H; i++) diff += panel[i] != fb[i];
    if (diff) {
      if (bad < 5) printf("  %d ms: %ld pixels on the panel differ from the frame\n", ms, diff);
      bad++;
      panel = fb;  // resync, keep looking for more
    }
    memcpy(prev.data(), fb.data(), W * H * 2);
    frames++;
  }
  printf("%dx%d%s %-8s %-5s %-6s: %ld frames, %.1f rects/frame, %s\n", W, H, round ? " round" : "", INPUT_NAME[input], home == HOME_AXO ? "axo" : "card",
         tiles ? "tiles" : "double", frames, (double)rectsTotal / frames, bad ? "MISMATCH" : "ok");
  return bad ? 1 : 0;
}

int main() {
  int fail = 0;
  const int sizes[][3] = {{320, 170, 0}, {320, 240, 0}, {240, 240, 0}, {160, 128, 0}, {240, 240, 1}};
  for (auto& sz : sizes)
    for (int home = 0; home < HOME_COUNT; home++)
      for (int tiles = 0; tiles < 2; tiles++) fail |= run(sz[0], sz[1], sz[2], home, tiles, 7 + sz[1] + home + sz[2]);
  // the other controls: only the hints differ (and the edit direction), one pass each
  for (auto& sz : sizes)
    for (int input = 2; input <= 3; input++) fail |= run(sz[0], sz[1], sz[2], HOME_CARD, true, 11 + sz[1] + input, input);
  return fail;
}
