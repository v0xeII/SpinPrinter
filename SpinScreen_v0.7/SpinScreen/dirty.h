// Which parts of the frame changed? Compares the new frame with the previous one in bands of
// rows and returns rectangles to resend. Usually only Spinny and a few numbers move, so a few
// kilobytes go over SPI instead of the whole frame. Portable (also used by the PC benchmark).
#pragma once
#include <stdint.h>
#include <string.h>

struct DirtyRect { int x, y, w, h; };

static const int DIRTY_BAND = 8;  // rows per band

// returns the number of rectangles written to out (0 = nothing changed)
static inline int dirtyRects(const uint16_t* cur, const uint16_t* prev, int W, int H, DirtyRect* out, int maxOut) {
  int n = 0;
  int openY = -1, openX0 = 0, openX1 = 0;  // the rectangle being grown band by band
  const int words = W / 2;                 // compare two pixels at a time
  for (int y0 = 0; y0 < H; y0 += DIRTY_BAND) {
    int y1 = y0 + DIRTY_BAND > H ? H : y0 + DIRTY_BAND;
    int bx0 = words, bx1 = -1;
    for (int y = y0; y < y1; y++) {
      const uint32_t* a = (const uint32_t*)(cur + y * W);
      const uint32_t* b = (const uint32_t*)(prev + y * W);
      if (!memcmp(a, b, words * 4)) continue;
      int l = 0, r = words - 1;
      while (l < bx0 && a[l] == b[l]) l++;
      while (r > bx1 && a[r] == b[r]) r--;
      if (l < bx0) bx0 = l;
      if (r > bx1) bx1 = r;
    }
    bool dirty = bx1 >= 0;
    if (dirty) {
      int x0 = bx0 * 2, x1 = bx1 * 2 + 2;
      if (openY < 0) { openY = y0; openX0 = x0; openX1 = x1; }
      else { if (x0 < openX0) openX0 = x0; if (x1 > openX1) openX1 = x1; }
    }
    if ((!dirty || y1 == H) && openY >= 0) {  // close the rectangle
      int yEnd = dirty ? y1 : y0;
      if (n < maxOut) out[n++] = {openX0, openY, openX1 - openX0, yEnd - openY};
      else {  // out of slots: grow the last one
        DirtyRect& r = out[maxOut - 1];
        int rx1 = r.x + r.w, ry1 = r.y + r.h;
        if (openX0 < r.x) r.x = openX0;
        if (openX1 > rx1) rx1 = openX1;
        if (yEnd > ry1) ry1 = yEnd;
        r.w = rx1 - r.x;
        r.h = ry1 - r.y;
      }
      openY = -1;
    }
  }
  return n;
}
