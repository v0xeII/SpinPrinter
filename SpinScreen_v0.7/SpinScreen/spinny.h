// Spinny v2: a leucistic axolotl seen from the side, swimming. Drawn from anti-aliased
// shapes along an animated spine every frame (no bitmaps).
#pragma once
#include "gfx.h"

enum Mood { MOOD_IDLE, MOOD_HAPPY, MOOD_WORK, MOOD_SLEEPY, MOOD_SAD };

namespace pal {
static const uint16_t BG = hex(0x0A080B);
static const uint16_t PANEL = hex(0x19131A);
static const uint16_t LINE = hex(0x2A2029);
static const uint16_t PINK = hex(0xFF8DB5);
static const uint16_t HOT = hex(0xFF4F9A);
static const uint16_t LIGHT = hex(0xF7E3EC);
static const uint16_t DIM = hex(0x8C7380);
static const uint16_t FAINT = hex(0x4A3B44);
static const uint16_t DARK = hex(0x170910);
static const uint16_t WHITE = hex(0xFFFFFF);
// axolotl
static const uint16_t SKIN_D = hex(0xC97C98);
static const uint16_t SKIN = hex(0xEBB0C3);
static const uint16_t SKIN_L = hex(0xFAD9E3);
static const uint16_t FIN = hex(0xA8667D);  // membrane, pre-blended on the dark background
static const uint16_t GILL = hex(0xD4386C);
static const uint16_t GILL_L = hex(0xF0628F);
static const uint16_t GILL_D = hex(0x8E2349);
static const uint16_t EYE = hex(0x140A0F);
}  // namespace pal

// Colour morphs. Real axolotls come as leucistic (pink), golden albino, wild type and
// melanoid; blue is a fantasy one, and RGB cycles through the rainbow.
struct SpinnyPal { uint16_t skinD, skin, skinL, fin, gill, gillL, gillD, eye; };
enum { MORPH_PINK, MORPH_GOLD, MORPH_WILD, MORPH_BLACK, MORPH_BLUE, MORPH_RGB, MORPH_COUNT };

static inline uint16_t hsv(float h, float s, float v) {
  h = fmodf(h, 360.f);
  if (h < 0) h += 360.f;
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.f, 2.f) - 1)), m = v - c, r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
  return rgb((uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255), (uint8_t)((b + m) * 255));
}

static inline SpinnyPal spinnyPalette(int morph, float t) {
  SpinnyPal p;
  switch (morph) {
    case MORPH_GOLD:
      p = {hex(0xC99A4E), hex(0xEBC27A), hex(0xF8E2B0), 0, hex(0xE08A6A), hex(0xF2A98A), hex(0xA35C45), hex(0xA8404E)};
      break;
    case MORPH_WILD:
      p = {hex(0x4A4532), hex(0x6B6648), hex(0x8E8763), 0, hex(0x5A463E), hex(0x7A5F52), hex(0x372B26), hex(0x0E0B08)};
      break;
    case MORPH_BLACK:
      p = {hex(0x2A2830), hex(0x3B3843), hex(0x5A5666), 0, hex(0x463A4C), hex(0x625268), hex(0x221B28), hex(0x050406)};
      break;
    case MORPH_BLUE:
      p = {hex(0x6E86B8), hex(0x9DB4E6), hex(0xCFDDF7), 0, hex(0x5468C8), hex(0x7C8FE8), hex(0x36439A), hex(0x0B0E1A)};
      break;
    case MORPH_RGB: {
      float h = t * 50.f;
      p = {hsv(h, 0.5f, 0.78f), hsv(h, 0.36f, 0.95f), hsv(h, 0.16f, 1.f), 0,
           hsv(h + 60, 0.85f, 0.9f), hsv(h + 60, 0.6f, 1.f), hsv(h + 60, 0.9f, 0.55f), hex(0x0A0A12)};
      break;
    }
    default:
      p = {pal::SKIN_D, pal::SKIN, pal::SKIN_L, 0, pal::GILL, pal::GILL_L, pal::GILL_D, pal::EYE};
      break;
  }
  p.fin = mix565(pal::BG, p.skin, 115);  // membrane: skin colour seen through, on the dark background
  return p;
}

// ---------------------------------------------------------------------------------------
// Spinny v3: a procedural creature. The behaviour code (ui.cpp) only says where the HEAD
// should go and how fast. The head steers there like a real swimmer (limited turning, in 3D:
// to turn round it swims towards you and back), and the body follows as a chain of 15 joints.
// The picture is drawn from that chain, so bends, U-turns, dives and the brief head-on view in
// the middle of a turn all come out of the motion by themselves.
// ---------------------------------------------------------------------------------------
static const int SPN = 15;

struct SpinnyBody {
  float x[SPN], y[SPN], z[SPN];  // joints, head first (screen px; z = towards the viewer)
  float yaw = 0, pitch = 0, v = 0;  // heading in the x-z plane (0 = right, pi = left), speed px/s
  float phase = 0, lastT = -1;
  float blink = 0, nextBlink = 2.5f, blinkT = -1;
  float bubbleT = -10, nextBubble = 4;
  float mouth = 0, tuck = 0;
  bool init = false;
  float s = 1;  // scale the chain was built for
};

struct SpinnyGoal {
  float tx = 0, ty = 0;  // where the head wants to be
  float maxV = 45;       // how fast he may swim
  bool hover = false;    // close enough: stay, face sideways (and drift there without turning)
  float sink = 0;        // >0 while hovering: drop the whole body level at this speed (px/s)
  int face = 0;          // while hovering: +1 face right, -1 face left, 0 = keep the way he faces
  // walls for the body (his corner). The head is steered by the goal; the rest of the body
  // bends away from a wall (towards the viewer) instead of poking through it.
  float wx0 = -1e9f, wx1 = 1e9f, wy0 = -1e9f, wy1 = 1e9f;
};

// body thickness per joint (px at scale 1), shared by the physics walls and the drawing
static const float SP_R[SPN] = {5.6f, 10.6f, 12.9f, 12.7f, 9.3f, 9.5f, 10.3f, 10.1f, 9.1f, 7.6f, 5.8f, 4.2f, 2.9f, 1.8f, 0.9f};

static inline float spWrap(float a) {
  while (a > 3.14159265f) a -= 6.2831853f;
  while (a < -3.14159265f) a += 6.2831853f;
  return a;
}

static inline void spinnyPlace(SpinnyBody& b, float hx, float hy, float s, bool facingRight) {
  float seg = 108.f * s / (SPN - 1);
  b.yaw = facingRight ? 0 : 3.14159265f;
  for (int i = 0; i < SPN; i++) {
    b.x[i] = hx + (facingRight ? -1 : 1) * seg * i;
    b.y[i] = hy;
    b.z[i] = 0;
  }
  b.v = 0;
  b.pitch = 0;
  b.s = s;
  b.init = true;
}

static inline void spinnyHeadXY(const SpinnyBody& b, float& x, float& y) { x = b.x[0]; y = b.y[0]; }
static inline void spinnyCenter(const SpinnyBody& b, float& x, float& y) {
  x = (b.x[0] + b.x[SPN / 2] + b.x[SPN - 1]) / 3;
  y = (b.y[0] + b.y[SPN / 2] + b.y[SPN - 1]) / 3;
}
static inline bool spinnyFacingRight(const SpinnyBody& b) { return fcos(b.yaw) > 0; }

// physics: steer the head to the goal, drag the chain behind it
static inline void spinnyUpdate(SpinnyBody& b, const SpinnyGoal& g, float dt, float s) {
  if (!b.init) spinnyPlace(b, g.tx, g.ty, s, true);
  if (fabsf(b.s - s) > 0.002f) {  // scale changed (boot -> menu): rescale around the head
    float k = s / b.s;
    for (int i = 1; i < SPN; i++) {
      b.x[i] = b.x[0] + (b.x[i] - b.x[0]) * k;
      b.y[i] = b.y[0] + (b.y[i] - b.y[0]) * k;
      b.z[i] = b.z[0] + (b.z[i] - b.z[0]) * k;
    }
    b.s = s;
  }
  float dx = g.tx - b.x[0], dy = g.ty - b.y[0], dist = fsqrt(dx * dx + dy * dy);
  bool hover = g.hover || dist < 4;
  // desired heading: towards the goal; while hovering, sideways (profile) on the nearer side
  float yawD;
  if (hover && g.face) yawD = g.face > 0 ? 0.f : 3.14159265f;
  else if (hover || fabsf(dx) < 6) yawD = fcos(b.yaw) >= 0 ? 0.f : 3.14159265f;
  else yawD = atan2f(-b.z[0] * 0.05f, dx);
  float err = spWrap(yawD - b.yaw);
  if (fabsf(err) > 2.7f) err = (fcos(b.yaw) >= 0 ? 1.f : -1.f) * fabsf(err);  // turn round towards the viewer
  float rate = 3.2f;
  float turn = clampf(err * 4.f, -rate, rate) * dt;
  b.yaw = spWrap(b.yaw + turn);
  float pitchD = hover ? 0 : clampf(atan2f(dy, fmaxf(fabsf(dx), 12.f)), -0.55f, 0.55f);
  b.pitch += clampf(pitchD - b.pitch, -2.f * dt, 2.f * dt);
  // speed: arrive gently; keep a little speed while turning (you cannot turn on the spot)
  float vD = hover ? 0 : g.maxV * clampf(dist / 28.f, 0, 1);
  if (fabsf(err) > 0.25f) vD = fmaxf(vD, 14.f * s);
  b.v += (vD - b.v) * (1 - expf(-dt * 3.5f));
  float cp = fcos(b.pitch);
  b.x[0] += fcos(b.yaw) * cp * b.v * dt;
  b.z[0] += fsin(b.yaw) * cp * b.v * dt;
  b.y[0] += fsin(b.pitch) * b.v * dt;
  if (hover) {  // drift into the exact spot and back into the picture plane
    b.x[0] += dx * (1 - expf(-dt * 2.f));
    if (g.sink > 0) {  // sinking: the whole body goes down level, like a real axolotl settling
      float st = clampf(dy, -g.sink * dt, g.sink * dt);
      for (int i = 0; i < SPN; i++) b.y[i] += st;
    } else {
      b.y[0] += dy * (1 - expf(-dt * 2.f));
    }
  }
  b.z[0] *= expf(-dt * 1.2f);
  // the chain: each joint keeps its distance to the one in front, and slowly straightens
  float seg = 108.f * s / (SPN - 1);
  float pdx = -fcos(b.yaw) * cp, pdy = -fsin(b.pitch), pdz = -fsin(b.yaw) * cp;  // "behind the head"
  const float hx = pdx, hy = pdy, hz = pdz;
  float kStraight = clampf(dt * (hover ? 2.2f : 0.6f), 0, 1);  // at rest the body slowly lies out straight
  for (int i = 1; i < SPN; i++) {
    float ex = b.x[i] - b.x[i - 1], ey = b.y[i] - b.y[i - 1], ez = b.z[i] - b.z[i - 1];
    float l = fsqrt(ex * ex + ey * ey + ez * ez);
    if (l < 1e-4f) { ex = pdx; ey = pdy; ez = pdz; l = 1; }
    ex /= l; ey /= l; ez /= l;
    float k = clampf(dt * (i < 4 ? 6.f : 2.5f), 0, 1);  // stiff neck, softer tail
    ex += (pdx - ex) * k; ey += (pdy - ey) * k; ez += (pdz - ez) * k;
    ex += (hx - ex) * kStraight; ey += (hy - ey) * kStraight; ez += (hz - ez) * kStraight;
    l = fsqrt(ex * ex + ey * ey + ez * ez);
    pdx = ex / l; pdy = ey / l; pdz = ez / l;
    // a wall in the way: keep this joint just inside it and turn the rest of the segment
    // towards the viewer (it looks like the tail curling round in 3D, never like a cut)
    float r = SP_R[i] * s, wave = (1.2f + 6.f * (float)i * i / ((SPN - 1) * (SPN - 1))) * 1.2f * s;
    float nx = b.x[i - 1] + pdx * seg, ny = b.y[i - 1] + pdy * seg;
    bool hit = false;
    float wk = 1 - expf(-dt * 14.f);  // firm but not instant: no snap when the walls appear (boot -> menu)
    if (nx > g.wx1 - r || nx < g.wx0 + r) {
      float wx = nx > g.wx1 - r ? g.wx1 - r : g.wx0 + r;
      pdx += (clampf((wx - b.x[i - 1]) / seg, -1, 1) - pdx) * wk;
      hit = true;
    }
    if (ny > g.wy1 - r - wave || ny < g.wy0 + r + wave) {
      float wy = ny > g.wy1 - r - wave ? g.wy1 - r - wave : g.wy0 + r + wave;
      pdy += (clampf((wy - b.y[i - 1]) / seg, -1, 1) - pdy) * wk;
      hit = true;
    }
    if (hit) {
      float q = pdx * pdx + pdy * pdy;
      if (q > 1) { q = fsqrt(q); pdx /= q; pdy /= q; q = 1; }
      pdz = (pdz < 0 ? -1.f : 1.f) * fsqrt(1 - q);
    }
    b.x[i] = b.x[i - 1] + pdx * seg;
    b.y[i] = b.y[i - 1] + pdy * seg;
    b.z[i] = b.z[i - 1] + pdz * seg;
  }
  b.tuck += (clampf(b.v / (40.f * s), 0, 1) - b.tuck) * (1 - expf(-dt * 5));  // legs fold back at speed
}

static inline void drawSpinny(Canvas& c, SpinnyBody& b, float t, Mood mood, const SpinnyPal& P, float squash = 0,
                              float alpha = 1) {
  using namespace pal;
  const uint16_t SKIN_D = P.skinD, SKIN = P.skin, SKIN_L = P.skinL, FIN = P.fin;
  const uint16_t GILL = P.gill, GILL_L = P.gillL, GILL_D = P.gillD, EYE = P.eye;
  const int N = SPN;
  float s = b.s;
  float dt = b.lastT < 0 ? 0.016f : t - b.lastT;
  if (dt < 0 || dt > 0.2f) dt = 0.016f;
  b.lastT = t;
  float speed = (mood == MOOD_WORK ? 1.6f : (mood == MOOD_SLEEPY ? 0.3f : (mood == MOOD_HAPPY ? 1.15f : 0.6f)));
  b.phase += dt * (speed + b.v / (60.f * s));  // tail beats faster when he swims
  if (b.blinkT < 0 && t > b.nextBlink) b.blinkT = t;
  b.blink = 0;
  if (b.blinkT >= 0) {
    float u = (t - b.blinkT) / 0.16f;
    b.blink = u < 0.5f ? u * 2 : 2 - u * 2;
    if (u >= 1) { b.blinkT = -1; b.nextBlink = t + 2.5f + fmodf(t * 5.17f, 3.5f); }
  }
  bool eyeClosed = mood == MOOD_SLEEPY || b.blink > 0.6f;

  // ---- project the chain; a small wave runs down the body (stronger towards the tail)
  float px[N], py[N], pr[N], u[N];
  const float* R = SP_R;
  float amp0 = (mood == MOOD_SLEEPY ? 0.35f : 1.f) * (0.6f + 0.6f * b.tuck);
  float breathe = 1 + 0.025f * fsin(t * 1.7f) + 0.06f * squash;
  for (int i = 0; i < N; i++) {
    u[i] = (float)i / (N - 1);
    float w = fsin(6.2832f * (u[i] * 0.85f - b.phase)) * (1.2f + 6.f * u[i] * u[i]) * s * amp0 * (u[i] > 0.15f ? 1 : 0.2f);
    px[i] = b.x[i];
    py[i] = b.y[i] + w;
    pr[i] = R[i] * s * (i < 4 ? breathe : 1.f);
  }
  // local frame at a point along the body: "back" = towards the tail (shrinks when the body
  // points at you), "down" = belly side
  auto frame = [&](float uu, float& x, float& y, float& r, float& bx, float& by, float& dx_, float& dy_) {
    float f = clampf(uu, 0, 1) * (N - 1);
    int i = (int)f;
    if (i >= N - 1) i = N - 2;
    float k = f - i;
    x = px[i] + (px[i + 1] - px[i]) * k;
    y = py[i] + (py[i + 1] - py[i]) * k;
    r = pr[i] + (pr[i + 1] - pr[i]) * k;
    int i0 = i > 0 ? i - 1 : 0, i1 = i + 2 < N ? i + 2 : N - 1;
    float seg = 108.f * s / (N - 1) * (i1 - i0);
    bx = (px[i1] - px[i0]) / seg;
    by = (py[i1] - py[i0]) / seg;
    float l = fsqrt(bx * bx + by * by);
    if (l > 1.f) { bx /= l; by /= l; l = 1; }
    if (l > 0.2f) {
      float nx = bx / l, ny = by / l;
      if (nx >= 0) { dx_ = -ny; dy_ = nx; } else { dx_ = ny; dy_ = -nx; }
      if (l < 0.45f) {  // nearly head-on: belly is simply down
        float m = (l - 0.2f) / 0.25f;
        dx_ *= m; dy_ = dy_ * m + (1 - m);
      }
    } else { dx_ = 0; dy_ = 1; }
  };
  float A = alpha;
  float sway = fsin(b.phase * 6.2832f * 0.9f);

  // ---- limbs: splayed and paddling when slow, folded back along the body when swimming
  auto limb = [&](float uu, float side, float phaseOff, uint16_t col, float a) {
    float x, y, r, bx, by, nx, ny;
    frame(uu, x, y, r, bx, by, nx, ny);
    float k = fsin(b.phase * 6.2832f + phaseOff) * 0.35f * (1 - b.tuck);
    float sx = x + bx * side * 3 * s + nx * r * 0.55f, sy = y + by * side * 3 * s + ny * r * 0.55f;
    float ku = (-4 + 5 * k) * (1 - b.tuck) + 6 * b.tuck, kv = 7 * (1 - b.tuck) + 3.5f * b.tuck;
    float fu = (-5 + 3 * k) * (1 - b.tuck) + 6.5f * b.tuck, fv = 3.5f * (1 - b.tuck) + 0.8f * b.tuck;
    float ex = sx + (bx * ku + nx * kv) * s, ey = sy + (by * ku + ny * kv) * s;
    float fx = ex + (bx * fu + nx * fv) * s, fy = ey + (by * fu + ny * fv) * s;
    Canvas::Seg sg[5];
    sg[0] = Canvas::seg(sx, sy, ex, ey, 3.2f * s, 2.2f * s);
    sg[1] = Canvas::seg(ex, ey, fx, fy, 2.2f * s, 1.3f * s);
    for (int j = -1; j <= 1; j++) {
      float tu = 2.6f, tv = j * 1.6f + 0.6f;  // toes point forward-down
      sg[3 + j] = Canvas::seg(fx, fy, fx + (-bx * tu + nx * tv) * s, fy + (-by * tu + ny * tv) * s, 0.75f * s, 0.75f * s);
    }
    c.tapers(sg, 5, col, a);
  };
  limb(0.27f, 1, 1.6f, SKIN_D, A);
  limb(0.6f, 1, -0.4f, SKIN_D, A);

  // ---- gills: three rami per side, each with fine filaments; flare when happy, droop when sad
  float droop = mood == MOOD_SAD ? 0.55f : (mood == MOOD_HAPPY ? -0.18f : 0.f);
  float flare = 1 + 0.06f * fsin(t * 1.7f + 0.5f);  // they breathe
  auto gills = [&](float dus, float dvs, uint16_t stem, uint16_t fil, float a) {
    float hx, hy, hr, bx, by, nx, ny;
    frame(0.2f, hx, hy, hr, bx, by, nx, ny);
    static const float ang[3] = {-1.2f, -0.62f, -0.08f}, len[3] = {17, 18, 15};
    auto P2 = [&](float ox, float oy, float au, float av, float& X, float& Y) {  // au: towards tail, av: down
      X = ox + bx * au + nx * av;
      Y = oy + by * au + ny * av;
    };
    static Canvas::Seg stems[6], front[24], back[24];  // static: keep them off the 8 KB task stack
    static float fl4[24][4], bl4[24][4];               // the same filaments as plain lines
    bool thin = 1.1f * s < 1.25f;                      // hair-thin at this size: draw them as lines
    for (int g = 0; g < 3; g++) {
      float an = ang[g] + droop * (1 - g * 0.3f) + fsin(t * 2.1f + g * 1.1f) * 0.08f + sway * 0.05f;
      float bxp, byp;
      P2(hx, hy, (dus - 2 + g * 1.6f) * s, -hr * 0.62f + g * hr * 0.33f + dvs * s, bxp, byp);
      float l = len[g] * s * flare;
      float ca = fcos(an), sa = fsin(an);
      float exp_, eyp;
      P2(bxp, byp, ca * l, sa * l, exp_, eyp);
      float bend = 2.6f * s;
      float mxp, myp;
      P2((bxp + exp_) / 2, (byp + eyp) / 2, -ca * bend, -sa * bend, mxp, myp);
      stems[g * 2] = Canvas::seg(bxp, byp, mxp, myp, 1.5f * s, 1.05f * s);
      stems[g * 2 + 1] = Canvas::seg(mxp, myp, exp_, eyp, 1.05f * s, 0.6f * s);
      for (int k = 1; k <= 8; k++) {
        float q = k / 8.6f;
        float ax = q < 0.5f ? bxp + (mxp - bxp) * q * 2 : mxp + (exp_ - mxp) * (q * 2 - 1);
        float ay = q < 0.5f ? byp + (myp - byp) * q * 2 : myp + (eyp - myp) * (q * 2 - 1);
        float fl = (1.6f + 2.4f * fsin(q * 2.9f)) * s * flare;
        float fa = an + 1.25f + fsin(t * 3.3f + k * 0.7f + g) * 0.15f;
        float tx_, ty_;
        P2(ax, ay, fcos(fa) * fl, fsin(fa) * fl, tx_, ty_);
        front[g * 8 + k - 1] = Canvas::seg(ax, ay, tx_, ty_, 0.55f * s, 0.55f * s);
        fl4[g * 8 + k - 1][0] = ax; fl4[g * 8 + k - 1][1] = ay; fl4[g * 8 + k - 1][2] = tx_; fl4[g * 8 + k - 1][3] = ty_;
        float fb = an - 1.1f;
        P2(ax, ay, fcos(fb) * fl * 0.55f, fsin(fb) * fl * 0.55f, tx_, ty_);
        back[g * 8 + k - 1] = Canvas::seg(ax, ay, tx_, ty_, 0.5f * s, 0.5f * s);
        bl4[g * 8 + k - 1][0] = ax; bl4[g * 8 + k - 1][1] = ay; bl4[g * 8 + k - 1][2] = tx_; bl4[g * 8 + k - 1][3] = ty_;
      }
    }
    if (thin) for (auto& l : bl4) c.line(l[0], l[1], l[2], l[3], 1.0f * s, fil, a * 0.75f);
    else c.tapers(back, 24, fil, a * 0.75f);  // filaments on the far side of the stems
    c.tapers(stems, 6, stem, a);
    if (thin) for (auto& l : fl4) c.line(l[0], l[1], l[2], l[3], 1.1f * s, fil, a);
    else c.tapers(front, 24, fil, a);
  };
  gills(3.f, -1.5f, GILL_D, GILL_D, A);

  // ---- fins: dorsal crest + tail fin
  auto finH = [&](float uu, bool top) {
    if (top) return uu < 0.28f ? 0.f : (uu < 0.78f ? (uu - 0.28f) / 0.5f * 9.f : 9.f - (uu - 0.78f) / 0.22f * 6.5f);
    return uu < 0.58f ? 0.f : (uu < 0.86f ? (uu - 0.58f) / 0.28f * 7.f : 7.f - (uu - 0.86f) / 0.14f * 5.f);
  };
  static Canvas::Seg fins[2 * N];
  int nf = 0;
  for (int i = 3; i < N - 1; i += 2) {  // every other joint: smooth enough for a fin, half the work
    int j = i + 2 < N ? i + 2 : N - 1;
    float x0, y0, r0_, b0x, b0y, n0x, n0y, x1, y1, r1_, b1x, b1y, n1x, n1y;
    frame(u[i], x0, y0, r0_, b0x, b0y, n0x, n0y);
    frame(u[j], x1, y1, r1_, b1x, b1y, n1x, n1y);
    for (int side = 0; side < 2; side++) {
      float h0 = finH(u[i], side == 0) * s, h1 = finH(u[j], side == 0) * s;
      if (h0 < 0.3f && h1 < 0.3f) continue;
      float sg = side == 0 ? -1.f : 1.f;
      float r0 = (pr[i] + h0) / 2, r1 = (pr[j] + h1) / 2;
      float o0 = sg * (pr[i] + h0 - r0), o1 = sg * (pr[j] + h1 - r1);
      fins[nf++] = Canvas::seg(px[i] + n0x * o0, py[i] + n0y * o0, px[j] + n1x * o1, py[j] + n1y * o1, r0, r1);
    }
  }
  c.tapers(fins, nf, FIN, A);

  // ---- body: dark base, lighter top layer, highlight streak (lit from above)
  static Canvas::Seg body[N];
  for (int i = 0; i < N - 1; i++) body[i] = Canvas::seg(px[i], py[i], px[i + 1], py[i + 1], pr[i], pr[i + 1]);
  c.tapers(body, N - 1, SKIN_D, A);
  // the inner layers only show inside the outline, so every other joint is enough
  // (half the pieces, about 40 % less work, no visible difference)
  static const int J2[9] = {0, 1, 2, 4, 6, 8, 10, 12, 14};
  for (int k = 0; k < 8; k++) {
    int i = J2[k], j = J2[k + 1];
    body[k] = Canvas::seg(px[i], py[i] - pr[i] * 0.22f, px[j], py[j] - pr[j] * 0.22f, pr[i] * 0.8f, pr[j] * 0.8f);
  }
  c.tapers(body, 8, SKIN, A);
  for (int k = 0; k < 6; k++) {
    int i = J2[k], j = J2[k + 1];
    body[k] = Canvas::seg(px[i], py[i] - pr[i] * 0.42f, px[j], py[j] - pr[j] * 0.42f, pr[i] * 0.42f, pr[j] * 0.42f);
  }
  c.tapers(body, 6, SKIN_L, A * 0.85f);
  float hx, hy, hr, hbx, hby, hnx, hny;
  frame(0.11f, hx, hy, hr, hbx, hby, hnx, hny);
  c.fillEllipse(hx + hbx * -1 * s, hy - hr * 0.62f, 6 * s * (0.4f + 0.6f * fabsf(hbx)), 1.6f * s, WHITE, 0.45f * A);

  // ---- near side
  limb(0.27f, -1, 0.f, SKIN, A);
  limb(0.6f, -1, -2.f, SKIN, A);
  gills(0.f, 0.f, GILL, GILL_L, A);

  // ---- face: one eye in profile, both when he looks at you mid-turn
  {
    float ex, ey, er, bx, by, nx, ny;
    frame(0.075f, ex, ey, er, bx, by, nx, ny);
    float side = fsqrt(bx * bx + by * by);  // 1 = profile, 0 = head-on
    auto eye = [&](float x, float y) {
      if (eyeClosed) c.arc(x, y - 0.6f * s, 2.4f * s, 1.0f * s, 0.4f, 2.74f, EYE, A);
      else {
        c.fillCircle(x, y, 2.5f * s, EYE, A);
        c.fillCircle(x - 0.8f * s, y - 0.9f * s, 0.85f * s, WHITE, 0.9f * A);
      }
    };
    float fx = ex - bx * 1.2f * s, fy = ey - by * 1.2f * s - er * 0.12f;
    if (side > 0.55f) eye(fx, fy);
    else {  // head-on: two eyes either side of the head
      float spread = er * 0.62f * (1 - side / 0.55f) + 0.01f;
      eye(fx - spread, fy);
      eye(fx + spread, fy);
    }
    // mouth: a soft line, opens to eat or gulp air
    float m = b.mouth;
    if (m > 0.05f) {
      float mx = px[0] + (px[1] - px[0]) * 0.35f, my = py[0] + 2.6f * s;
      c.fillEllipse(mx, my, 3.2f * s * (0.6f + 0.4f * m) * (0.5f + 0.5f * side), 2.4f * s * m, EYE, A);
    }
  }

  // ---- bubbles from the snout
  if (t > b.nextBubble) {
    b.bubbleT = t;
    b.nextBubble = t + (mood == MOOD_SLEEPY ? 2.2f : 4.f) + fmodf(t * 3.7f, 2.f);
  }
  float bt = t - b.bubbleT;
  if (bt >= 0 && bt < 2.2f) {
    float sx = px[0] + (px[0] - px[1]) * 0.5f, sy = py[0] - 2 * s;
    for (int k = 0; k < 3; k++) {
      float tt = bt - k * 0.22f;
      if (tt < 0) continue;
      float r = (1.0f + k * 0.5f) * s;
      c.ring(sx + fsin(tt * 5 + k) * 1.5f * s, sy - tt * 22 * s, r, 0.8f * s, LIGHT, A * 0.7f * clampf(1 - tt / 2.f, 0, 1));
    }
  }
}
