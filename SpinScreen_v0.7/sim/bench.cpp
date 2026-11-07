// Bench: runs the real UI with the real frame pacing and the real dirty-rectangle logic and
// reports, per second: frames drawn, pixels computed, bytes that would go over SPI.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include "config.h"
#include "dirty.h"
#include "gfx.h"
#include "ui.h"
static float cur = 0, tgt = 0; static double ang = 0;
static void hs(const char*) {} static void hsp(bool o, float r) { tgt = o ? r : 0; } static void hb(int) {}
static void hsv(const Settings&) {} static float hr() { return cur; } static double ha() { return ang; }
static void hc(const Settings&) {} static void hz() { ang = 0; } static void hm(double, float) {}
int main(int argc, char** argv) {
  int W = 320, H = 170;
  if (argc > 1) sscanf(argv[1], "%dx%d", &W, &H);
  std::vector<uint16_t> fb(W * H), prev(W * H, 0xFFFF);
  Canvas c(W, H, fb.data());
  UiHal hal{hs, hsp, hb, hsv, hr, ha, hc, hz, hm};
  Settings s{4.0f, 0, 80, 16, 200, 6.f, 0, 1.0f, 1, 1, 0};
  ui_init(hal, s, 0);
  struct Ph { const char* name; int t0, t1; long frames = 0, sdf = 0, bytes = 0; double cpu = 0; };
  std::vector<Ph> ph = {{"menu, calm", 4000, 9000},          {"menu, scrolling", 9000, 12000},
                        {"motor page, spinning", 14000, 19000}, {"menu, Spinny swims", 25000, 45000},
                        {"asleep (>60 s)", 80000, 90000}};
  int want = 60;
  long nextUs = 0;
  for (int ms = 0; ms < 90000; ms++) {
    if (ms % 1000 == 0) ui_line("PING", ms);
    bool key = false;
    if (ms >= 9000 && ms < 12000 && ms % 400 == 0) { ui_key((ms / 400) % 2 ? K_DOWN : K_UP, ms); key = true; }
    if (ms == 12500) { ui_key(K_DOWN, ms); ui_key(K_DOWN, ms); } if (ms == 12600) ui_key(K_OK, ms);  // Сервис
    if (ms == 13000) ui_key(K_DOWN, ms); if (ms == 13200) ui_key(K_OK, ms);                         // Мотор
    if (ms == 14000) hsp(true, 6);
    if (ms == 19000) { hsp(false, 0); ui_key(K_BACK, ms); ui_key(K_BACK, ms); }
    cur += (tgt - cur) * 0.002f; ang += cur * 6 * 0.001;
    long t = ms * 1000L;  // same pacing as main.cpp: an exact microsecond grid
    if (key && nextUs > t + 16667) nextUs = t;
    while (nextUs < t + 1000) {
      long frameT = nextUs;
      nextUs += 1000000 / want;
#ifdef GFX_STATS
      gstats = GfxStats();
#endif
      clock_t c0 = clock();
      want = ui_frame(c, (uint32_t)(frameT / 1000), (frameT % 1000) / 1000.f);
      double cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
      DirtyRect r[12];
      int n = dirtyRects(fb.data(), prev.data(), W, H, r, 12);
      long b = 0;
      for (int i = 0; i < n; i++) b += r[i].w * r[i].h * 2 + 12;
      prev = fb;
      for (auto& p : ph)
        if (frameT / 1000 >= p.t0 && frameT / 1000 < p.t1) {
          p.frames++; p.bytes += b; p.cpu += cpu;
#ifdef GFX_STATS
          p.sdf += gstats.sdf + gstats.blend;
#endif
        }
    }
  }
  printf("%dx%d\n", W, H);
  for (auto& p : ph) {
    double sec = (p.t1 - p.t0) / 1000.0;
    printf("  %-22s %4.0f fps  %6.0f SDF px/frame  %5.1f KB/frame over SPI  (PC: %.3f ms/frame)\n", p.name,
           p.frames / sec, p.frames ? (double)p.sdf / p.frames : 0, p.frames ? p.bytes / 1024.0 / p.frames : 0,
           p.frames ? p.cpu * 1000 / p.frames : 0);
  }
}
