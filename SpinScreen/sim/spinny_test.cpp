// Spinny gallery: every colour and mood, drawn with the real code (writes spinny.ppm).
// build: g++ -O2 -std=c++17 spinny_test.cpp -o spinny_test
#include <stdio.h>

#include <vector>

#include "../SpinScreen/gfx.h"
#include "../SpinScreen/spinny.h"

int main() {
  const int W = 600, H = 260;
  std::vector<uint16_t> m(W * H);
  Canvas c(W, H, m.data());
  c.clear(pal::BG);
  const Mood moods[6] = {MOOD_IDLE, MOOD_HAPPY, MOOD_SAD, MOOD_SLEEPY, MOOD_WORK, MOOD_IDLE};
  for (int k = 0; k < 6; k++) {
    float hx = 150 + (k % 3) * 190, hy = 70 + (k / 3) * 120, s = 1.25f;
    SpinnyBody b;
    spinnyPlace(b, hx, hy, s, true);
    SpinnyGoal g;
    g.tx = hx;
    g.ty = hy;
    g.hover = true;
    for (int i = 0; i < 120; i++) spinnyUpdate(b, g, 1 / 60.f, s);  // settle
    for (int i = 0; i < 40; i++) drawSpinny(c, b, i / 60.f + k, moods[k], spinnyPalette(k, 1.f), 0, 0);  // advance clocks
    drawSpinny(c, b, 1.f + k, moods[k], spinnyPalette(k, 1.f));
  }
  FILE* f = fopen("spinny.ppm", "wb");
  fprintf(f, "P6 %d %d 255\n", W, H);
  for (auto p : m) {
    fputc((p >> 11) << 3, f);
    fputc(((p >> 5) & 63) << 2, f);
    fputc((p & 31) << 3, f);
  }
  fclose(f);
}
