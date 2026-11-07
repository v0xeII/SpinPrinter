// Arduino builds with -Os (small code). The drawing code is the hot path, so this file is
// compiled for speed instead.
#pragma GCC optimize("O2")
// ST7789 driver over SPI with DMA.
//  * two framebuffers when RAM allows (the last frame is kept to see what changed)
//  * pixels go out through small staging buffers, byte-swapped on the way, so drawing the next
//    frame never waits for the SPI
//  * only what changed goes over SPI (dirty.h), so a calm screen costs almost nothing
//  * 170x320 and 240x240 panels (config.h)
#ifdef ARDUINO
#include "display.h"

#include <Arduino.h>

#include "config.h"
#include "dirty.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

static const int W = DISPLAY_W, H = DISPLAY_H;
static const int CHUNK_PX = 4096;  // pixels per DMA transaction (8 KB): small, so two 240x240 buffers still fit
static spi_device_handle_t dev;
static uint16_t* fbs[2] = {nullptr, nullptr};
// Staging: a ring of chunks the pixels are copied into (byte-swapped) on their way to the DMA.
// As many as RAM allows (up to 80 KB), so a whole frame's changes usually fit: display_push()
// then returns at once and the next frame is drawn while the last one is still going out.
static const int MAX_RING = 10;
static uint16_t* ring[MAX_RING];
static uint32_t ringSeq[MAX_RING];  // the transaction that last used each chunk
static int nRing = 0, ringNext = 0;
static uint32_t qSeq = 0, doneSeq = 0;  // transactions queued / finished (they finish in order)
static int cur = 0, inflight = 0;
static bool dbl = false, first = true;
static spi_transaction_t trs[64];
static int trNext = 0;
static int offX = 0, offY = 0;
// one buffer only: a hash per tile (DIRTY_BAND rows x TILE_W columns) of the last frame sent, so
// only the changed columns of a band go out, not the whole width (a pulsing dot used to cost a
// full-width band: 10 KB per frame instead of 0.5 KB)
static const int TILE_W = 16;
static const int TILES_X = (DISPLAY_W + TILE_W - 1) / TILE_W;
static uint32_t tileHash[(DISPLAY_H + DIRTY_BAND - 1) / DIRTY_BAND][TILES_X];

static void IRAM_ATTR preCb(spi_transaction_t* t) {
  gpio_set_level((gpio_num_t)PIN_TFT_DC, (int)(intptr_t)t->user);
}

static void waitAll();
static void sendBytes(const uint8_t* d, int n, int dc) {
  waitAll();  // a polling send may not overtake queued transactions
  spi_transaction_t t = {};
  t.length = 8 * n;
  t.user = (void*)(intptr_t)dc;
  if (n <= 4) {
    t.flags = SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, d, n);
  } else {
    t.tx_buffer = d;
  }
  spi_device_polling_transmit(dev, &t);
}
static void cmd(uint8_t c) { sendBytes(&c, 1, 0); }
static void cmdData(uint8_t c, const uint8_t* d, int n) {
  cmd(c);
  if (n) sendBytes(d, n, 1);
}

static void waitOne() {
  spi_transaction_t* r;
  spi_device_get_trans_result(dev, &r, portMAX_DELAY);
  inflight--;
  doneSeq++;
}
static void waitAll() {
  while (inflight > 0) waitOne();
}
// queue one transaction; dc = level of the D/C line (0 command, 1 data). Returns its number.
static uint32_t queueTx(const void* data, int bytes, int dc) {
  if (inflight >= (int)(sizeof(trs) / sizeof(trs[0]))) waitOne();
  spi_transaction_t& t = trs[trNext];
  trNext = (trNext + 1) % (int)(sizeof(trs) / sizeof(trs[0]));
  memset(&t, 0, sizeof t);
  t.length = bytes * 8;
  t.user = (void*)(intptr_t)dc;
  if (bytes <= 4) {
    t.flags = SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, data, bytes);
  } else {
    t.tx_buffer = data;
  }
  spi_device_queue_trans(dev, &t, portMAX_DELAY);
  inflight++;
  return ++qSeq;
}

static void window(int x, int y, int w, int h) {
  uint16_t x0 = x + offX, x1 = x + w - 1 + offX, y0 = y + offY, y1 = y + h - 1 + offY;
  uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  // queued, not polled: no need to wait for the pixels of the previous rectangle first
  static const uint8_t CASET = 0x2A, RASET = 0x2B, RAMWR = 0x2C;
  queueTx(&CASET, 1, 0);
  queueTx(ca, 4, 1);
  queueTx(&RASET, 1, 0);
  queueTx(ra, 4, 1);
  queueTx(&RAMWR, 1, 0);  // memory write
}

bool display_init() {
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RES, OUTPUT);
  digitalWrite(PIN_TFT_RES, HIGH);
  delay(5);
  digitalWrite(PIN_TFT_RES, LOW);
  delay(20);
  digitalWrite(PIN_TFT_RES, HIGH);
  delay(120);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_TFT_SDA;
  bus.miso_io_num = -1;
  bus.sclk_io_num = PIN_TFT_SCL;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = CHUNK_PX * 2;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t dc = {};
  dc.clock_speed_hz = TFT_SPI_HZ;
  dc.mode = TFT_SPI_MODE;
  dc.spics_io_num = PIN_TFT_CS;
  dc.queue_size = sizeof(trs) / sizeof(trs[0]);
  dc.pre_cb = preCb;
  if (spi_bus_add_device(SPI2_HOST, &dc, &dev) != ESP_OK) return false;

  // orientation + where the visible area sits in the controller's 240x320 memory
  uint8_t madctl;
#if TFT_PANEL == PANEL_240x240
  static const uint8_t MAD[4] = {0x00, 0x60, 0xC0, 0xA0};
  static const int OX[4] = {0, 0, 0, 80}, OY[4] = {0, 0, 80, 0};
#else
  static const uint8_t MAD[4] = {0x00, 0x60, 0xC0, 0xA0};
  static const int OX[4] = {35, 0, 35, 0}, OY[4] = {0, 35, 0, 35};
#endif
  madctl = MAD[TFT_ROTATION & 3];
  offX = OX[TFT_ROTATION & 3];
  offY = OY[TFT_ROTATION & 3];
#ifdef TFT_OFFSET_X
  offX = TFT_OFFSET_X;
#endif
#ifdef TFT_OFFSET_Y
  offY = TFT_OFFSET_Y;
#endif

  cmd(0x01);  // software reset
  delay(150);
  cmd(0x11);  // sleep out
  delay(120);
  uint8_t colmod = 0x55;  // 16-bit colour
  cmdData(0x3A, &colmod, 1);
  cmdData(0x36, &madctl, 1);
  cmd(TFT_INVERT ? 0x21 : 0x20);
  cmd(0x13);  // normal display mode

  // the small buffers first, so a big panel's framebuffers can never starve them;
  // the second framebuffer only if enough RAM stays free for the rest of the firmware
  // (two 320x240 buffers are 300 KB)
  size_t sz = W * H * 2;
  for (nRing = 0; nRing < 2; nRing++) {
    ring[nRing] = (uint16_t*)heap_caps_malloc(CHUNK_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!ring[nRing]) return false;
    ringSeq[nRing] = 0;
  }
  fbs[0] = (uint16_t*)heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  fbs[1] = (uint16_t*)heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (fbs[1] && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 48 * 1024) {
    heap_caps_free(fbs[1]);
    fbs[1] = nullptr;
  }
  if (!fbs[0]) return false;
  // more staging chunks while RAM stays comfortable for the rest of the firmware
  while (nRing < MAX_RING && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 56 * 1024) {
    ring[nRing] = (uint16_t*)heap_caps_malloc(CHUNK_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!ring[nRing]) break;
    ringSeq[nRing++] = 0;
  }
  dbl = fbs[1] != nullptr;
  memset(fbs[0], 0, sz);
  first = true;
  display_push();  // black frame before the backlight comes on
  waitAll();
  cmd(0x29);  // display on
  return true;
}

uint16_t* display_buffer() { return fbs[cur]; }
bool display_double_buffered() { return dbl; }

static inline void swapCopy(uint16_t* dst, const uint16_t* src, int n) {
  if (!(((uintptr_t)dst | (uintptr_t)src) & 3)) {  // two pixels at a time
    const uint32_t* s = (const uint32_t*)src;
    uint32_t* d = (uint32_t*)dst;
    for (int i = 0; i < n / 2; i++) {
      uint32_t v = s[i];
      d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
    if (n & 1) dst[n - 1] = __builtin_bswap16(src[n - 1]);
  } else {
    for (int i = 0; i < n; i++) dst[i] = __builtin_bswap16(src[i]);
  }
}

static uint32_t sendRect(const uint16_t* b, const DirtyRect& r) {
  window(r.x, r.y, r.w, r.h);
  // rows are copied into the staging ring and byte-swapped on the way (the panel wants big-endian
  // pixels). The framebuffers stay untouched, so the next frame can be drawn and compared while
  // these pieces are still going out.
  int rowsPer = CHUNK_PX / r.w;
  for (int y = r.y; y < r.y + r.h; y += rowsPer) {
    int rows = r.y + r.h - y < rowsPer ? r.y + r.h - y : rowsPer;
    int k = ringNext;
    ringNext = (ringNext + 1) % nRing;
    while (doneSeq < ringSeq[k]) waitOne();  // this chunk is still being sent
    uint16_t* dst = ring[k];
    for (int j = 0; j < rows; j++) swapCopy(dst + j * r.w, b + (y + j) * W + r.x, r.w);
    ringSeq[k] = queueTx(dst, rows * r.w * 2, 1);
  }
  return (uint32_t)r.w * r.h * 2;
}

uint32_t display_push() {
  uint16_t* b = fbs[cur];
  static const int MAXRECTS = 24;
  static DirtyRect rects[MAXRECTS];
  int n;
  if (first) {
    rects[0] = {0, 0, W, H};
    n = 1;
  } else if (dbl) {  // the other buffer = what is on the screen now
    n = dirtyRects(b, fbs[cur ^ 1], W, H, rects, MAXRECTS);
  } else {  // one buffer only: compare tile hashes with the last frame sent
    // Each band is cut into runs of changed tiles (a clean gap of GAP tiles or more splits them),
    // and a run continues the rectangle above it when it has the same columns. So a card on the
    // left and a pill on the right go out as two narrow rectangles, not one full-width one.
    const int GAP = 3, MAXRUN = 4;
    struct Open { int x0, x1, y0; bool used; } open[MAXRUN];
    int nOpen = 0;
    n = 0;
    auto emit = [&](int x0, int x1, int y0, int y1) {
      if (n < MAXRECTS) { rects[n++] = {x0, y0, x1 - x0, y1 - y0}; return; }
      DirtyRect& r = rects[MAXRECTS - 1];  // out of slots: grow the last one
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
      uint32_t h[TILES_X];
      for (int t = 0; t < TILES_X; t++) h[t] = 2166136261u;
      for (int y = y0; y < y1; y++) {
        const uint32_t* q = (const uint32_t*)(b + y * W);
        for (int t = 0, i = 0; t < TILES_X; t++) {
          int e = (t + 1) * TILE_W / 2 < W / 2 ? (t + 1) * TILE_W / 2 : W / 2;
          uint32_t v = h[t];
          for (; i < e; i++) v = (v ^ q[i]) * 16777619u;
          h[t] = v;
        }
      }
      // runs of changed tiles in this band
      int rx0[MAXRUN], rx1[MAXRUN], nr = 0, last = -100;
      for (int t = 0; t < TILES_X; t++) {
        if (h[t] == tileHash[band][t]) continue;
        tileHash[band][t] = h[t];
        if (nr > 0 && (t - last <= GAP || nr == MAXRUN)) rx1[nr - 1] = t;
        else { rx0[nr] = t; rx1[nr] = t; nr++; }
        last = t;
      }
      for (int k = 0; k < nOpen; k++) open[k].used = false;
      for (int r = 0; r < nr; r++) {
        int x0 = rx0[r] * TILE_W, x1 = (rx1[r] + 1) * TILE_W > W ? W : (rx1[r] + 1) * TILE_W;
        int k = 0;
        while (k < nOpen && !(open[k].x0 == x0 && open[k].x1 == x1 && !open[k].used)) k++;
        if (k < nOpen) { open[k].used = true; continue; }  // same columns as the band above: grow it
        if (nOpen < MAXRUN) open[nOpen++] = {x0, x1, y0, true};
        else emit(x0, x1, y0, y1);
      }
      for (int k = 0; k < nOpen;) {  // rectangles that did not go on: close them
        if (open[k].used) { k++; continue; }
        emit(open[k].x0, open[k].x1, open[k].y0, y0);
        open[k] = open[--nOpen];
      }
    }
    for (int k = 0; k < nOpen; k++) emit(open[k].x0, open[k].x1, open[k].y0, H);
  }
  uint32_t bytes = 0;
  for (int i = 0; i < n; i++) bytes += sendRect(b, rects[i]);
  first = false;
  // The screen now shows (or is about to show) exactly this buffer: draw the next frame into
  // the other one and compare it against this. The last staging pieces may still be in flight;
  // that is fine, the DMA never reads the framebuffers.
  if (dbl) cur ^= 1;
  return bytes;
}

void display_backlight(int pct) {
  static bool attached = false;
  if (!attached) {
    ledcAttachChannel(PIN_TFT_BLK, 20000, 8, 0);
    attached = true;
  }
  float u = pct / 100.f;
  int duty = (int)(u * u * 255);
  if (duty < 6) duty = 6;
  ledcWrite(PIN_TFT_BLK, duty);
}
#endif
