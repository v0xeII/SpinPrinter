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
static uint16_t* stage[2] = {nullptr, nullptr};  // contiguous copies of narrow rectangles
static int cur = 0, inflight = 0;
static bool dbl = false, first = true;
static spi_transaction_t trs[8];
static int trNext = 0;
static int offX = 0, offY = 0;
static uint32_t bandHash[(DISPLAY_H + DIRTY_BAND - 1) / DIRTY_BAND];

static void IRAM_ATTR preCb(spi_transaction_t* t) {
  gpio_set_level((gpio_num_t)PIN_TFT_DC, (int)(intptr_t)t->user);
}

static void sendBytes(const uint8_t* d, int n, int dc) {
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
}
static void waitAll() {
  while (inflight > 0) waitOne();
}
static void queue(const void* data, int pixels) {
  if (inflight >= (int)(sizeof(trs) / sizeof(trs[0]))) waitOne();
  spi_transaction_t& t = trs[trNext];
  trNext = (trNext + 1) % (int)(sizeof(trs) / sizeof(trs[0]));
  memset(&t, 0, sizeof t);
  t.length = pixels * 16;
  t.tx_buffer = data;
  t.user = (void*)1;
  spi_device_queue_trans(dev, &t, portMAX_DELAY);
  inflight++;
}

static void window(int x, int y, int w, int h) {
  uint16_t x0 = x + offX, x1 = x + w - 1 + offX, y0 = y + offY, y1 = y + h - 1 + offY;
  uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
  cmdData(0x2A, ca, 4);
  cmdData(0x2B, ra, 4);
  cmd(0x2C);  // memory write
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

  size_t sz = W * H * 2;
  fbs[0] = (uint16_t*)heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  fbs[1] = (uint16_t*)heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  stage[0] = (uint16_t*)heap_caps_malloc(CHUNK_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  stage[1] = (uint16_t*)heap_caps_malloc(CHUNK_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!fbs[0] || !stage[0] || !stage[1]) return false;
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
  waitAll();  // commands go out directly, nothing may be queued
  window(r.x, r.y, r.w, r.h);
  // rows are copied into a staging buffer (two, used in turn) and byte-swapped on the way
  // (the panel wants big-endian pixels). The framebuffers stay untouched, so the next frame can
  // be drawn and compared while the last pieces are still going out.
  int rowsPer = CHUNK_PX / r.w, s = 0;
  for (int y = r.y; y < r.y + r.h; y += rowsPer) {
    int rows = r.y + r.h - y < rowsPer ? r.y + r.h - y : rowsPer;
    if (inflight >= 2) waitOne();  // the staging buffer we are about to fill is free again
    uint16_t* dst = stage[s];
    for (int k = 0; k < rows; k++) swapCopy(dst + k * r.w, b + (y + k) * W + r.x, r.w);
    queue(dst, rows * r.w);
    s ^= 1;
  }
  return (uint32_t)r.w * r.h * 2;
}

uint32_t display_push() {
  uint16_t* b = fbs[cur];
  static DirtyRect rects[12];
  int n;
  if (first) {
    rects[0] = {0, 0, W, H};
    n = 1;
  } else if (dbl) {  // the other buffer = what is on the screen now
    n = dirtyRects(b, fbs[cur ^ 1], W, H, rects, 12);
  } else {  // one buffer only: remember a hash per band instead of the whole frame
    n = 0;
    int open = -1;
    for (int band = 0, y0 = 0; y0 < H; band++, y0 += DIRTY_BAND) {
      int y1 = y0 + DIRTY_BAND > H ? H : y0 + DIRTY_BAND;
      uint32_t h = 2166136261u;
      const uint32_t* q = (const uint32_t*)(b + y0 * W);
      for (int i = 0; i < (y1 - y0) * W / 2; i++) h = (h ^ q[i]) * 16777619u;
      bool ch = h != bandHash[band];
      bandHash[band] = h;
      if (ch && open < 0) open = y0;
      if ((!ch || y1 == H) && open >= 0 && n < 12) {
        int ye = ch ? y1 : y0;
        rects[n++] = {0, open, W, ye - open};
        open = -1;
      }
    }
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
