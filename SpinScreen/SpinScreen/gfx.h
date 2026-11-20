// SpinScreen: tiny anti-aliased graphics on an RGB565 framebuffer.
// Pure C++ (no Arduino), so the same code runs on the ESP32 and in the PC simulator.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

struct Glyph {
  uint32_t cp;   // unicode code point
  uint32_t off;  // offset into bitmap (4-bit alpha, 2 px per byte, rows byte-aligned)
  uint8_t w, h;
  int8_t xo, yo;  // bitmap position relative to pen (x) and baseline (y)
  uint8_t adv;
};
struct Font {
  const uint8_t* bmp;
  const Glyph* gl;
  uint16_t n;
  uint8_t lineH, ascent;
};

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t hex(uint32_t c) { return rgb(c >> 16, (c >> 8) & 255, c & 255); }

// a = 0..256
static inline uint16_t mix565(uint16_t bg, uint16_t fg, uint32_t a) {
  if (a >= 256) return fg;
  if (a == 0) return bg;
  // spread RGB565 into 0x07E0F81F layout so all channels blend in one multiply
  uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81F;
  uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81F;
  uint32_t a5 = a >> 3;  // 0..32
  uint32_t r = ((f * a5 + b * (32 - a5)) >> 5) & 0x07E0F81F;
  return (uint16_t)(r | (r >> 16));
}
static inline uint16_t lerpColor(uint16_t a, uint16_t b, float t) {
  if (t <= 0) return a;
  if (t >= 1) return b;
  return mix565(a, b, (uint32_t)(t * 256));
}

static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

// Fast maths for the per-pixel code. On the ESP32-S3 sqrtf(), fminf(), fmaxf() and float
// division are library calls (the FPU has no single instruction for them): ~100 cycles for a
// square root. These inline versions cost a few cycles and are accurate to ~5e-6, far below
// what anti-aliasing can show.
static inline float fmn(float a, float b) { return a < b ? a : b; }
static inline float fmx(float a, float b) { return a > b ? a : b; }
static inline float frsqrt(float x) {  // 1/sqrt(x), x > 0
  union { float f; uint32_t i; } u;
  u.f = x;
  u.i = 0x5F375A86u - (u.i >> 1);
  float y = u.f, hx = 0.5f * x;
  y = y * (1.5f - hx * y * y);
  y = y * (1.5f - hx * y * y);
  return y;
}
static inline float fsqrt(float x) { return x > 1e-20f ? x * frsqrt(x) : 0.f; }
static inline int ifloor(float x) {
  int i = (int)x;
  return i - (x < (float)i);
}
// sine / cosine for animation (error < 0.001, no library call)
static inline float fsin(float x) {
  // (no names like PI here: Arduino.h #defines PI and TWO_PI)
  x -= 6.2831853f * (float)ifloor((x + 3.14159265f) * 0.15915494f);  // to [-pi, pi)
  float y = 1.27323954f * x - 0.40528473f * x * fabsf(x);
  return 0.225f * (y * fabsf(y) - y) + y;
}
static inline float fcos(float x) { return fsin(x + 1.57079633f); }

#ifdef GFX_STATS  // the PC simulator counts work per frame
struct GfxStats { long sdf = 0, blend = 0, fill = 0; };
inline GfxStats gstats;
#define GSTAT(f, n) (gstats.f += (n))
#else
#define GSTAT(f, n) ((void)0)
#endif

class Canvas {
 public:
  int W, H;
  uint16_t* buf;
  int cx0 = 0, cy0 = 0, cx1, cy1;  // clip rect [x0,x1) [y0,y1)

  Canvas(int w, int h, uint16_t* mem) : W(w), H(h), buf(mem), cx1(w), cy1(h) {}

  // Optional transform for the shape functions (used by Spinny: turn around, tilt).
  // A point p on screen samples the shape at q = c + S^-1 R(-a) (p - c).
  bool xf = false;
  float xcx = 0, xcy = 0, xsx = 1, xsy = 1, xisx = 1, xisy = 1, xca = 1, xsa = 0, xdist = 1;
  void transform(float cx, float cy, float sx, float sy, float angle) {
    xf = true;
    xcx = cx; xcy = cy; xsx = sx; xsy = sy;
    xisx = 1.f / sx; xisy = 1.f / sy;  // multiply per pixel instead of dividing (no hardware divide)
    xca = cosf(angle); xsa = sinf(angle);
    float ax = fabsf(sx), ay = fabsf(sy);
    xdist = ax < ay ? ax : ay;  // distances shrink with the smaller scale (keeps edges crisp)
  }
  void noTransform() { xf = false; }

  void clip(int x0, int y0, int x1, int y1) {
    cx0 = x0 < 0 ? 0 : x0; cy0 = y0 < 0 ? 0 : y0;
    cx1 = x1 > W ? W : x1; cy1 = y1 > H ? H : y1;
  }
  void noClip() { cx0 = 0; cy0 = 0; cx1 = W; cy1 = H; }

  void clear(uint16_t c) {
    GSTAT(fill, W * H);
    uint32_t v = c | ((uint32_t)c << 16), *p = (uint32_t*)buf, n = (uint32_t)(W * H) / 2;
    for (uint32_t i = 0; i < n; i++) p[i] = v;
    if ((W * H) & 1) buf[W * H - 1] = c;
  }
  inline void blend(int x, int y, uint16_t c, uint32_t a) {
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1 || a == 0) return;
    uint16_t& p = buf[y * W + x];
    p = mix565(p, c, a);
  }

  void fillRect(int x, int y, int w, int h, uint16_t c, float alpha = 1) {
    int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
    int x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
    uint32_t a = (uint32_t)(clampf(alpha, 0, 1) * 256);
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++) buf[yy * W + xx] = mix565(buf[yy * W + xx], c, a);
  }

  // Vertical gradient (used for backgrounds / glows)
  void gradientV(int x, int y, int w, int h, uint16_t top, uint16_t bottom) {
    for (int yy = 0; yy < h; yy++) fillRect(x, y + yy, w, 1, lerpColor(top, bottom, (float)yy / (h > 1 ? h - 1 : 1)));
  }

  // ---- signed-distance shapes, anti-aliased (float coords, pixel centres at +0.5) ----
  // slack: extra safety for shapes whose distance is only approximate (ellipse)
  template <typename SDF>
  void shape(float bx0, float by0, float bx1, float by1, uint16_t c, float alpha, SDF sdf, float slack = 0) {
    if (xf) {  // bounding box of the transformed corners
      float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
      for (int k = 0; k < 4; k++) {
        float qx = ((k & 1) ? bx1 : bx0) - xcx, qy = ((k & 2) ? by1 : by0) - xcy;
        qx *= xsx; qy *= xsy;
        float px = xcx + qx * xca - qy * xsa, py = xcy + qx * xsa + qy * xca;
        mnx = fmn(mnx, px); mxx = fmx(mxx, px); mny = fmn(mny, py); mxy = fmx(mxy, py);
      }
      bx0 = mnx; bx1 = mxx; by0 = mny; by1 = mxy;
    }
    int x0 = ifloor(bx0) - 1, y0 = ifloor(by0) - 1;
    int x1 = -ifloor(-bx1) + 1, y1 = -ifloor(-by1) + 1;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    float A = clampf(alpha, 0, 1) * 256.f;
    uint32_t Ai = (uint32_t)A;
    auto dist = [&](float px, float py) -> float {
      if (!xf) return sdf(px, py);
      float dx = px - xcx, dy = py - xcy;
      float rx = dx * xca + dy * xsa, ry = -dx * xsa + dy * xca;
      return sdf(xcx + rx * xisx, xcy + ry * xisy) * xdist;
    };
    // Row marching: a distance field changes by at most 1 per pixel, so a pixel 3.7 px outside
    // the shape means the next 3 are outside too (skip them), and one 3.7 px inside means the
    // next 3 are fully covered (fill them). Only the anti-aliased edge pixels get computed.
    float sk = 0.5f + slack;
#ifdef GFX_REFERENCE  // test builds: every pixel computed, to check the fast paths draw the same
    sk = 1e9f;
#endif
    for (int y = y0; y < y1; y++) {
      float py = y + 0.5f;
      uint16_t* row = buf + y * W;
      int x = x0;
      while (x < x1) {
        GSTAT(sdf, 1);
        float d = dist(x + 0.5f, py);
        if (d >= 0.5f) {
          int k = d - sk > 0 ? (int)(d - sk) + 1 : 1;
          x += k;
        } else if (d <= -0.5f) {
          int k = -d - sk > 0 ? (int)(-d - sk) + 1 : 1;
          int e = x + k < x1 ? x + k : x1;
          GSTAT(fill, e - x);
          if (Ai >= 256) for (; x < e; x++) row[x] = c;
          else for (; x < e; x++) row[x] = mix565(row[x], c, Ai);
        } else {
          row[x] = mix565(row[x], c, (uint32_t)((0.5f - d) * A));
          x++;
        }
      }
    }
  }

  void fillCircle(float cx, float cy, float r, uint16_t c, float alpha = 1) {
    shape(cx - r, cy - r, cx + r, cy + r, c, alpha, [=](float x, float y) {
      return fsqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
    });
  }
  void ring(float cx, float cy, float r, float th, uint16_t c, float alpha = 1) {
    float R = r + th;
    shape(cx - R, cy - R, cx + R, cy + R, c, alpha, [=](float x, float y) {
      return fabsf(fsqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r) - th * 0.5f;
    });
  }
  // approximate ellipse SDF (good enough for small sprites)
  void fillEllipse(float cx, float cy, float rx, float ry, uint16_t c, float alpha = 1) {
    if (rx < 0.3f || ry < 0.3f) return;
    float irx = 1.f / rx, iry = 1.f / ry, mn = fmn(rx, ry);
    shape(cx - rx, cy - ry, cx + rx, cy + ry, c, alpha, [=](float x, float y) {
      float dx = (x - cx) * irx, dy = (y - cy) * iry;
      float k0 = fsqrt(dx * dx + dy * dy);
      if (k0 < 1e-4f) return -mn;
      float ex = dx * irx, ey = dy * iry;
      return k0 * (k0 - 1.f) * frsqrt(ex * ex + ey * ey);
    }, 1.0f);
  }
  void fillRoundRect(float x, float y, float w, float h, float r, uint16_t c, float alpha = 1) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    float hx = w / 2, hy = h / 2, mx = x + hx, my = y + hy;
    shape(x, y, x + w, y + h, c, alpha, [=](float px, float py) {
      float qx = fabsf(px - mx) - hx + r, qy = fabsf(py - my) - hy + r;
      float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
      return fsqrt(ox * ox + oy * oy) + fmn(fmx(qx, qy), 0.f) - r;
    });
  }
  void strokeRoundRect(float x, float y, float w, float h, float r, float th, uint16_t c, float alpha = 1) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    float hx = w / 2, hy = h / 2, mx = x + hx, my = y + hy;
    shape(x - th, y - th, x + w + th, y + h + th, c, alpha, [=](float px, float py) {
      float qx = fabsf(px - mx) - hx + r, qy = fabsf(py - my) - hy + r;
      float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
      float d = fsqrt(ox * ox + oy * oy) + fmn(fmx(qx, qy), 0.f) - r;
      return fabsf(d) - th * 0.5f;
    });
  }
  // thick line with round caps
  void capsule(float ax, float ay, float bx, float by, float r, uint16_t c, float alpha = 1) {
    float vx = bx - ax, vy = by - ay, L2 = vx * vx + vy * vy, iL2 = L2 > 0 ? 1.f / L2 : 0;
    shape(fmn(ax, bx) - r, fmn(ay, by) - r, fmx(ax, bx) + r, fmx(ay, by) + r, c, alpha,
          [=](float x, float y) {
            float t = clampf(((x - ax) * vx + (y - ay) * vy) * iL2, 0, 1);
            float dx = x - ax - vx * t, dy = y - ay - vy * t;
            return fsqrt(dx * dx + dy * dy) - r;
          });
  }
  // Thin anti-aliased line (Xiaolin Wu), for hair-thin details like the gill filaments:
  // two pixel blends per step instead of a distance field. width <= ~1.2 px.
  void line(float x0, float y0, float x1, float y1, float width, uint16_t c, float alpha) {
    float A = clampf(alpha * fmn(width, 1.f), 0, 1) * 256.f;
    if (A < 1) return;
    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) { float t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dx = x1 - x0, grad = dx > 1e-3f ? (y1 - y0) / dx : 0;
    int xa = ifloor(x0), xb = ifloor(x1);
    for (int i = xa; i <= xb; i++) {
      float xc = i + 0.5f;
      float cov = fmn(xc + 0.5f, x1) - fmx(xc - 0.5f, x0);  // how much of this column the line spans
      if (dx <= 1e-3f) cov = 1;
      if (cov <= 0) continue;
      float yy = y0 + grad * (xc - x0) - 0.5f;
      int j = ifloor(yy);
      float f = yy - j;
      uint32_t a0 = (uint32_t)((1 - f) * cov * A), a1 = (uint32_t)(f * cov * A);
      if (steep) { blend(j, i, c, a0); blend(j + 1, i, c, a1); }
      else { blend(i, j, c, a0); blend(i, j + 1, c, a1); }
    }
  }

  // ---- many tapered capsules drawn as ONE shape (their union) ----
  // Spinny is made of chains of overlapping capsules. Drawn one by one, every pixel where they
  // overlap is computed (and blended) several times, and the seams get a faint double edge.
  // Drawn as a union, each pixel is computed once against only the 1-3 pieces near it.
  struct Seg {
    float ax, ay, vx, vy, h, ih, kx, ky, ra, rb;
    float x0, y0, x1, y1;  // bounding box
    bool circle;
  };
  static Seg seg(float ax, float ay, float bx, float by, float ra, float rb) {
    Seg s;
    float R = fmx(ra, rb);
    s.x0 = fmn(ax, bx) - R; s.x1 = fmx(ax, bx) + R;
    s.y0 = fmn(ay, by) - R; s.y1 = fmx(ay, by) + R;
    s.ax = ax; s.ay = ay; s.vx = bx - ax; s.vy = by - ay; s.ra = ra; s.rb = rb;
    s.h = s.vx * s.vx + s.vy * s.vy;
    float b = ra - rb;
    s.circle = s.h < 1e-4f || b * b >= s.h * 0.98f;  // one end swallows the other
    if (s.circle) {
      if (rb > ra) { s.ax = bx; s.ay = by; s.ra = rb; }
      return s;
    }
    s.ih = 1.f / s.h;
    s.kx = fsqrt(s.h - b * b);
    s.ky = b;
    return s;
  }
  __attribute__((always_inline)) static inline float segDist(const Seg& s, float x, float y) {
    float px = x - s.ax, py = y - s.ay;
    if (s.circle) return fsqrt(px * px + py * py) - s.ra;
    float qx = fabsf(px * s.vy - py * s.vx) * s.ih, qy = (px * s.vx + py * s.vy) * s.ih;
    float k = s.kx * qy - s.ky * qx, n = qx * qx + qy * qy;
    if (k < 0) return fsqrt(s.h * n) - s.ra;
    if (k > s.kx) return fsqrt(s.h * (n + 1 - 2 * qy)) - s.rb;
    return s.kx * qx + s.ky * qy - s.ra;
  }
  void tapers(const Seg* sg, int n, uint16_t c, float alpha) {
    if (alpha <= 0 || n <= 0) return;
    if (n > 64) n = 64;
    float fx0 = 1e9f, fy0 = 1e9f, fx1 = -1e9f, fy1 = -1e9f;
    for (int i = 0; i < n; i++) {
      fx0 = fmn(fx0, sg[i].x0); fy0 = fmn(fy0, sg[i].y0);
      fx1 = fmx(fx1, sg[i].x1); fy1 = fmx(fy1, sg[i].y1);
    }
    int x0 = ifloor(fx0) - 1, y0 = ifloor(fy0) - 1, x1 = -ifloor(-fx1) + 1, y1 = -ifloor(-fy1) + 1;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 <= x0 || y1 <= y0) return;
    float A = clampf(alpha, 0, 1) * 256.f;
    uint32_t Ai = (uint32_t)A;
    uint8_t cand[64], cand2[64];
    // test the pieces near a block; returns: 0 = nothing there, 1 = fully covered, 2 = edge
    auto test = [&](int bx, int by, int ex, int ey, const uint8_t* in, int nin, uint8_t* out, int& nout) {
      float cxp = (bx + ex) * 0.5f, cyp = (by + ey) * 0.5f;
      float hw = (ex - bx - 1) * 0.5f, hh = (ey - by - 1) * 0.5f;
      float R = fsqrt(hw * hw + hh * hh) + 0.5f;
      nout = 0;
      float dd[64];
      for (int k = 0; k < nin; k++) {
        const Seg& s = sg[in[k]];
        if (s.x1 < bx || s.x0 > ex || s.y1 < by || s.y0 > ey) continue;  // nowhere near
        GSTAT(sdf, 1);
        float d = segDist(s, cxp, cyp);
        if (d <= -R) return 1;
        if (d < R) {  // keep, nearest first (then the first piece usually decides "inside")
          int j = nout++;
          while (j > 0 && dd[j - 1] > d) { dd[j] = dd[j - 1]; out[j] = out[j - 1]; j--; }
          dd[j] = d;
          out[j] = in[k];
        }
      }
      return nout ? 2 : 0;
    };
    auto solid = [&](int xa, int ya, int xb, int yb) {
      GSTAT(fill, (xb - xa) * (yb - ya));
      for (int y = ya; y < yb; y++) {
        uint16_t* row = buf + y * W;
        if (Ai >= 256) for (int x = xa; x < xb; x++) row[x] = c;
        else for (int x = xa; x < xb; x++) row[x] = mix565(row[x], c, Ai);
      }
    };
    for (int i = 0; i < n; i++) cand[i] = (uint8_t)i;
    int nc;
#ifdef GFX_REFERENCE
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++) {
        float d = 1e9f;
        for (int k = 0; k < n; k++) d = fmn(d, segDist(sg[k], x + 0.5f, y + 0.5f));
        uint16_t& p = buf[y * W + x];
        if (d < 0.5f) p = mix565(p, c, d <= -0.5f ? Ai : (uint32_t)((0.5f - d) * A));
      }
    return;
#endif
    for (int by = y0; by < y1; by += 8) {
      int ey = by + 8 < y1 ? by + 8 : y1;
      for (int bx = x0; bx < x1; bx += 8) {
        int ex = bx + 8 < x1 ? bx + 8 : x1;
        int r8 = test(bx, by, ex, ey, cand, n, cand2, nc);
        if (r8 == 0) continue;
        if (r8 == 1) { solid(bx, by, ex, ey); continue; }
        // edge block: march along each row against the pieces near this block only
        for (int y = by; y < ey; y++) {
          float py = y + 0.5f;
          uint16_t* row = buf + y * W;
          int x = bx;
          while (x < ex) {
            float d = 1e9f;
            for (int k = 0; k < nc; k++) {
              GSTAT(sdf, 1);
              float e = segDist(sg[cand2[k]], x + 0.5f, py);
              if (e < d) d = e;
              if (d <= -0.5f) break;  // already fully inside: the rest cannot change that
            }
            if (d >= 0.5f) {
              x += (int)(d - 0.5f) + 1;
            } else if (d <= -0.5f) {
              int e = x + (int)(-d - 0.5f) + 1;
              if (e > ex) e = ex;
              if (Ai >= 256) for (; x < e; x++) row[x] = c;
              else for (; x < e; x++) row[x] = mix565(row[x], c, Ai);
            } else {
              row[x] = mix565(row[x], c, (uint32_t)((0.5f - d) * A));
              x++;
            }
          }
        }
      }
    }
  }

  // tapered capsule: radius ra at A, rb at B (Inigo Quilez' uneven capsule)
  void taper(float ax, float ay, float bx, float by, float ra, float rb, uint16_t c, float alpha = 1) {
    float vx = bx - ax, vy = by - ay, h = vx * vx + vy * vy;
    if (h < 1e-4f) { fillCircle(ax, ay, fmx(ra, rb), c, alpha); return; }
    float b = ra - rb;
    if (b * b >= h * 0.98f) { capsule(ax, ay, bx, by, fmx(ra, rb), c, alpha); return; }
    float cx = fsqrt(h - b * b), cy = b, R = fmx(ra, rb), ih = 1.f / h;
    shape(fmn(ax, bx) - R, fmn(ay, by) - R, fmx(ax, bx) + R, fmx(ay, by) + R, c, alpha,
          [=](float x, float y) {
            float px = x - ax, py = y - ay;
            float qx = fabsf(px * vy - py * vx) * ih, qy = (px * vx + py * vy) * ih;
            float k = cx * qy - cy * qx, m = cx * qx + cy * qy, n = qx * qx + qy * qy;
            if (k < 0) return fsqrt(h * n) - ra;
            if (k > cx) return fsqrt(h * (n + 1 - 2 * qy)) - rb;
            return m - ra;
          });
  }
  // arc from angle a0 to a1 (radians, 0 = right, clockwise on screen since y goes down), round caps
  void arc(float cx, float cy, float r, float th, float a0, float a1, uint16_t c, float alpha = 1) {
    if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
    if (a1 - a0 >= 6.2831f) { ring(cx, cy, r, th, c, alpha); return; }
    float mid = (a0 + a1) / 2, half = (a1 - a0) / 2;
    float ex0 = cx + cosf(a0) * r, ey0 = cy + sinf(a0) * r;
    float ex1 = cx + cosf(a1) * r, ey1 = cy + sinf(a1) * r;
    float mx = cosf(mid), my = sinf(mid), ch = cosf(half);  // inside the arc <=> angle to mid <= half
    float R = r + th;
    float hth = th * 0.5f;
    shape(cx - R, cy - R, cx + R, cy + R, c, alpha, [=](float x, float y) {
      float dx = x - cx, dy = y - cy;
      float len = fsqrt(dx * dx + dy * dy);
      if (dx * mx + dy * my >= ch * len) return fabsf(len - r) - hth;
      float d0 = (x - ex0) * (x - ex0) + (y - ey0) * (y - ey0);
      float d1 = (x - ex1) * (x - ex1) + (y - ey1) * (y - ey1);
      return fsqrt(fmn(d0, d1)) - hth;
    });
  }

  // soft radial glow, alpha (1 - d²/r²)⁴ (≈ (1 - d/r)²)
  // The panel blends in 32 steps, so a faint glow is just a few flat rings: per row work out
  // where each ring starts and ends (one square root per ring) and fill spans. No per-pixel maths.
  void glow(float cx, float cy, float r, uint16_t c, float alpha) {
    int Ai = (int)(clampf(alpha, 0, 1) * 256);
#ifdef GFX_REFERENCE
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) continue;
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy, u = 1.f - (dx * dx + dy * dy) / (r * r);
        if (u <= 0) continue;
        uint32_t a = (uint32_t)(u * u * u * u * Ai);
        buf[y * W + x] = mix565(buf[y * W + x], c, a);
      }
    return;
#endif
    int Lmax = (Ai > 255 ? 256 : Ai) >> 3;
    if (Lmax <= 0 || r <= 0) return;
    float t[33];  // squared radius where blend level L starts
    for (int L = 1; L <= Lmax; L++) {
      float q = fsqrt(fsqrt(8.f * L / Ai));  // (8L/A)^(1/4)
      t[L] = r * r * (1 - q);
    }
    int y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    int xa[34], xb[34];
    for (int y = y0; y < y1; y++) {
      float dy = y + 0.5f - cy, dy2 = dy * dy;
      int n = 0;  // levels reached in this row
      for (int L = 1; L <= Lmax && t[L] > dy2; L++) {
        float half = fsqrt(t[L] - dy2);
        xa[L] = -ifloor(-(cx - half - 0.5f));
        xb[L] = ifloor(cx + half - 0.5f);
        if (xa[L] < cx0) xa[L] = cx0;
        if (xb[L] > cx1 - 1) xb[L] = cx1 - 1;
        n = L;
      }
      uint16_t* row = buf + y * W;
      for (int L = 1; L <= n; L++) {
        uint32_t a = (uint32_t)(L * 8);
        int ia = L < n ? xa[L + 1] : xb[L] + 1;  // the next ring inside takes over there
        int ib = L < n ? xb[L + 1] : xb[L];
        for (int x = xa[L]; x < ia && x <= xb[L]; x++) row[x] = mix565(row[x], c, a);
        for (int x = (ib + 1 > xa[L] ? ib + 1 : xa[L]); x <= xb[L]; x++) row[x] = mix565(row[x], c, a);
        GSTAT(blend, xb[L] - xa[L] + 1);
      }
    }
  }

  // ---- text ----
  static uint32_t nextCp(const char*& s) {
    uint8_t c = (uint8_t)*s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) { uint32_t r = (c & 0x1F) << 6; r |= (*s++ & 0x3F); return r; }
    if ((c & 0xF0) == 0xE0) {
      uint32_t r = (c & 0x0F) << 12; r |= (*s++ & 0x3F) << 6; r |= (*s++ & 0x3F); return r;
    }
    s += 3;
    return '?';
  }
  static const Glyph* find(const Font& f, uint32_t cp) {
    int lo = 0, hi = f.n - 1;
    while (lo <= hi) {
      int m = (lo + hi) >> 1;
      if (f.gl[m].cp == cp) return &f.gl[m];
      if (f.gl[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return nullptr;
  }
  static int textWidth(const Font& f, const char* s) {
    int w = 0;
    while (*s) {
      const Glyph* g = find(f, nextCp(s));
      if (g) w += g->adv;
    }
    return w;
  }
  // y = baseline. returns pen x after the text
  int text(const Font& f, int x, int y, const char* s, uint16_t c, float alpha = 1) {
    uint32_t A = (uint32_t)(clampf(alpha, 0, 1) * 256);
    while (*s) {
      const Glyph* g = find(f, nextCp(s));
      if (!g) g = find(f, '?');
      if (!g) continue;
      if (A && g->w) {
        const uint8_t* p = f.bmp + g->off;
        int rowBytes = (g->w + 1) >> 1;
        int gx = x + g->xo, gy = y + g->yo;
        // clip the glyph box once, then plain loops (glyph pixels are most of the UI's work)
        int xs = gx < cx0 ? cx0 - gx : 0, xe = gx + g->w > cx1 ? cx1 - gx : g->w;
        int ys = gy < cy0 ? cy0 - gy : 0, ye = gy + g->h > cy1 ? cy1 - gy : g->h;
        uint32_t k = 17 * A;
        for (int yy = ys; yy < ye; yy++) {
          const uint8_t* row = p + yy * rowBytes;
          uint16_t* dst = buf + (gy + yy) * W + gx;
          for (int xx = xs; xx < xe; xx++) {
            uint8_t b = row[xx >> 1];
            if (!b) { xx |= 1; continue; }  // two empty pixels
            uint32_t v = (xx & 1) ? (b & 15) : (b >> 4);
            if (v) dst[xx] = mix565(dst[xx], c, (v * k) >> 8);
          }
        }
      }
      x += g->adv;
    }
    return x;
  }
  void textCentered(const Font& f, int cx, int y, const char* s, uint16_t c, float alpha = 1) {
    text(f, cx - textWidth(f, s) / 2, y, s, c, alpha);
  }
  void textRight(const Font& f, int rx, int y, const char* s, uint16_t c, float alpha = 1) {
    text(f, rx - textWidth(f, s), y, s, c, alpha);
  }
};
