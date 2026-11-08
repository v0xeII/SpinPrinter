// Spinny stress test: the real UI for 20 simulated minutes with uneven frames (30/60 fps,
// stalls like a flash write or a blocked USB write), key presses, prints, errors, sleep.
// Reports when Spinny's body leaves his corner or jerks (moves much more than his speed allows).
//   make spinny_stress && ./spinny_stress [seed]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "config.h"
#include "gfx.h"
#include "spinny.h"
#include "ui.h"

const SpinnyBody& ui_probe_body();
void ui_probe_area(float* a);
extern float ui_probe_sa;
static const char* SCR[] = {"boot", "menu", "setup", "print", "done", "about", "connect"};
static long outBy[7][4];  // screen x edge (left, right, top, bottom)

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
static unsigned rs = 1;
static float rnd() { rs = rs * 1103515245u + 12345u; return ((rs >> 8) & 0xFFFFFF) / 16777216.f; }

int main(int argc, char** argv) {
  unsigned seed = argc > 1 ? atoi(argv[1]) : 1;
  rs = seed;
  bool stalls = !(argc > 2 && !strcmp(argv[2], "nostall"));
  const int W = 320, H = 240;  // ./spinny_stress SEED [nostall|stall]
  std::vector<uint16_t> fb(W * H);
  Canvas c(W, H, fb.data());
  UiHal hal{hs, hsp, hb, hsv, hr, ha, hc, hz, hm};
  Settings s{4.0f, 0, 80, 16, 200, 6.f, 0, 1.0f, 1, 1, 0};
  ui_init(hal, s, 0);
  double t = 0;
  long frames = 0, outs = 0, jerks = 0;
  double worstOut = 0, worstJerk = 0, worstOutT = 0, worstJerkT = 0;
  float px[SPN], py[SPN];
  bool have = false;
  double nextPing = 1, nextKey = 4, nextEvent = 20;
  int fps = 60;
  std::vector<double> flips;  // when he turned round (facing changed)
  bool facing = true;
  while (t < 1200) {
    // frame spacing: 60 or 30 fps, now and then a stall (NVS write, blocked USB write, …)
    double dt = 1.0 / fps;
    if (stalls && rnd() < 0.004f) dt += 0.03 + rnd() * 0.25;
    if (rnd() < 0.002f) fps = fps == 60 ? 30 : 60;
    t += dt;
    uint32_t ms = (uint32_t)(t * 1000);
    if (t > nextPing) { ui_line("PING", ms); nextPing += 1; }
    if (t > nextKey) {  // bursts of scrolling, sometimes OK / back
      float r = rnd();
      ui_key(r < 0.4f ? K_DOWN : r < 0.8f ? K_UP : r < 0.9f ? K_OK : K_BACK, ms);
      nextKey = t + (rnd() < 0.7f ? 0.15 + rnd() * 0.4 : 3 + rnd() * 90);
    }
    if (t > nextEvent) {  // a print, its end, an error, a message
      float r = rnd();
      if (r < 0.25f) { ui_line("STATE PRINT", ms); ui_line("PROG 10 60", ms); }
      else if (r < 0.5f) ui_line("STATE DONE", ms);
      else if (r < 0.7f) ui_line("STATE ERROR test", ms);
      else if (r < 0.85f) ui_line("STATE IDLE", ms);
      else ui_line("MSG Привет", ms);
      nextEvent = t + 10 + rnd() * 60;
    }
    cur += (tgt - cur) * 0.05f;
    ang += cur * 6 * dt;
    ui_frame(c, ms, 0);
    frames++;
    float a[6];
    ui_probe_area(a);
    const SpinnyBody& b = ui_probe_body();
    // R as in drawSpinny, plus a little for the wave and the gills
    static const float R[SPN] = {5.6f, 10.6f, 12.9f, 12.7f, 9.3f, 9.5f, 10.3f, 10.1f, 9.1f, 7.6f, 5.8f, 4.2f, 2.9f, 1.8f, 0.9f};
    float sc = a[4];
    int screen = (int)a[5];
    if (screen != 0 && ui_probe_sa > 0.05f) {  // visible, not the boot screen (he swims in from outside there)
      double e[4] = {0, 0, 0, 0};
      for (int i = 0; i < SPN; i++) {
        float r = R[i] * sc * 0.8f;
        e[0] = fmax(e[0], a[0] - (b.x[i] - r));
        e[1] = fmax(e[1], (b.x[i] + r) - a[1]);
        e[2] = fmax(e[2], a[2] - (b.y[i] - r));
        e[3] = fmax(e[3], (b.y[i] + r) - a[3]);
      }
      double out = fmax(fmax(e[0], e[1]), fmax(e[2], e[3]));
      for (int k = 0; k < 4; k++) if (e[k] > 2) outBy[screen][k]++;
      if (out > 2) { outs++; if (out > worstOut) { worstOut = out; worstOutT = t; } }
      static bool dumped[7][4];
      for (int k = 0; k < 4; k++)
        if (e[k] > 15 && !dumped[screen][k] && getenv("DUMP")) {  // a picture of it, for a human to look at
          dumped[screen][k] = true;
          char fn[64];
          snprintf(fn, sizeof fn, "out_%s_%d.ppm", SCR[screen], k);
          FILE* f = fopen(fn, "wb");
          fprintf(f, "P6 %d %d 255\n", W, H);
          for (auto p : fb) { fputc((p >> 11) << 3, f); fputc(((p >> 5) & 63) << 2, f); fputc((p & 31) << 3, f); }
          fclose(f);
          printf("   dump %s at %.1f s (edge %d by %.0f px)\n", fn, t, k, e[k]);
        }
    }
    if (have) {  // jerk: a joint moved far more than swimming could explain
      double mv = 0;
      for (int i = 0; i < SPN; i++) mv = fmax(mv, hypot(b.x[i] - px[i], b.y[i] - py[i]));
      double allowed = (fabs(b.v) + 40 * sc) * dt + 1.5;
      if (mv > allowed * 1.5) { jerks++; if (mv - allowed > worstJerk) { worstJerk = mv - allowed; worstJerkT = t; } }
    }
    bool f = spinnyFacingRight(b);
    if (f != facing && screen == 1) flips.push_back(t);  // menu only: on «Готово!» he does laps on purpose
    facing = f;
    for (int i = 0; i < SPN; i++) { px[i] = b.x[i]; py[i] = b.y[i]; }
    have = true;
  }
  printf("seed %u: %ld frames, out of his corner: %ld frames (worst %.1f px at %.0f s), jerks: %ld (worst +%.1f px at %.0f s)\n",
         seed, frames, outs, worstOut, worstOutT, jerks, worstJerk, worstJerkT);
  long dither = 0;  // turned round 4+ times within 8 s: back and forth
  for (size_t i = 3; i < flips.size(); i++)
    if (flips[i] - flips[i - 3] < 8) dither++;
  printf("   turned round in the menu %zu times in 20 min, back-and-forth bursts: %ld\n", flips.size(), dither);
  for (int k = 0; k < 7; k++)
    if (outBy[k][0] + outBy[k][1] + outBy[k][2] + outBy[k][3])
      printf("   %-8s left %ld  right %ld  top %ld  bottom %ld\n", SCR[k], outBy[k][0], outBy[k][1], outBy[k][2], outBy[k][3]);
  return 0;
}
