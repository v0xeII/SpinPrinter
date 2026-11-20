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

// One framebuffer only (no copy of the last frame to compare with): a hash per tile (DIRTY_BAND
// rows x DIRTY_TILE_W columns) of the last frame sent. Each band is cut into runs of changed tiles
// (a clean gap of more than DIRTY_GAP tiles splits them), and a run continues the rectangle above
// it when it has the same columns. So a card on the left and a pill on the right go out as two
// narrow rectangles, not one full-width band (a pulsing dot used to cost 10 KB a frame, now 0.5).
// tileHash: dirtyTileCount(W, H) words, kept between calls (start with zeros).
static const int DIRTY_TILE_W = 16, DIRTY_GAP = 3, DIRTY_MAXRUN = 4, DIRTY_MAX_TILES_X = 32;
static inline int dirtyTileCount(int W, int H) {
  return ((H + DIRTY_BAND - 1) / DIRTY_BAND) * ((W + DIRTY_TILE_W - 1) / DIRTY_TILE_W);
}
static inline int dirtyTiles(const uint16_t* b, int W, int H, uint32_t* tileHash, DirtyRect* out, int maxOut) {
  const int tilesX = (W + DIRTY_TILE_W - 1) / DIRTY_TILE_W;  // at most DIRTY_MAX_TILES_X
  struct Open { int x0, x1, y0; bool used; } open[DIRTY_MAXRUN];
  int nOpen = 0, n = 0;
  auto emit = [&](int x0, int x1, int y0, int y1) {
    if (n < maxOut) { out[n++] = {x0, y0, x1 - x0, y1 - y0}; return; }
    DirtyRect& r = out[maxOut - 1];  // out of slots: grow the last one
    int rx1 = r.x + r.w, ry1 = r.y + r.h;
    if (x0 < r.x) r.x = x0;
    if (y0 < r.y) r.y = y0;
    if (x1 > rx1) rx1 = x1;
    if (y1 > ry1) ry1 = y1;
    r.w = rx1 - r.x;
    r.h = ry1 - r.y;
  };
  for (int band = 0, y0 = 0; y0 < H; band++, y0 += DIRTY_BAND) {
    int y1 = y0 + DIRTY_BAND > H ? H : y0 + DIRTY_BAND;
    uint32_t h[DIRTY_MAX_TILES_X];
    for (int t = 0; t < tilesX; t++) h[t] = 2166136261u;
    for (int y = y0; y < y1; y++) {
      const uint32_t* q = (const uint32_t*)(b + y * W);
      for (int t = 0, i = 0; t < tilesX; t++) {
        int e = (t + 1) * DIRTY_TILE_W / 2 < W / 2 ? (t + 1) * DIRTY_TILE_W / 2 : W / 2;
        uint32_t v = h[t];
        // FNV alone ((v ^ w) * p) only carries a change upwards: the same top-bit change in two
        // rows of a tile cancelled out and the tile looked unchanged (stale pixels on Spinny's
        // edges, found by sim/dirty_test.cpp). Folding the top half down after each word fixes it;
        // every step is still a bijection, so a change in a single word is always seen.
        for (; i < e; i++) {
          v = (v ^ q[i]) * 16777619u;
          v ^= v >> 16;
        }
        h[t] = v;
      }
    }
    uint32_t* th = tileHash + band * tilesX;
    int rx0[DIRTY_MAXRUN], rx1[DIRTY_MAXRUN], nr = 0, last = -100;
    for (int t = 0; t < tilesX; t++) {
      if (h[t] == th[t]) continue;
      th[t] = h[t];
      if (nr > 0 && (t - last <= DIRTY_GAP || nr == DIRTY_MAXRUN)) rx1[nr - 1] = t;
      else { rx0[nr] = t; rx1[nr] = t; nr++; }
      last = t;
    }
    for (int k = 0; k < nOpen; k++) open[k].used = false;
    for (int r = 0; r < nr; r++) {
      int x0 = rx0[r] * DIRTY_TILE_W, x1 = (rx1[r] + 1) * DIRTY_TILE_W > W ? W : (rx1[r] + 1) * DIRTY_TILE_W;
      int k = 0;
      while (k < nOpen && !(open[k].x0 == x0 && open[k].x1 == x1 && !open[k].used)) k++;
      if (k < nOpen) { open[k].used = true; continue; }  // same columns as the band above: grow it
      if (nOpen < DIRTY_MAXRUN) open[nOpen++] = {x0, x1, y0, true};
      else emit(x0, x1, y0, y1);
    }
    for (int k = 0; k < nOpen;) {  // rectangles that did not go on: close them
      if (open[k].used) { k++; continue; }
      emit(open[k].x0, open[k].x1, open[k].y0, y0);
      open[k] = open[--nOpen];
    }
  }
  for (int k = 0; k < nOpen; k++) emit(open[k].x0, open[k].x1, open[k].y0, H);
  return n;
}
