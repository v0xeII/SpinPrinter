// Arduino builds with -Os (small code). The drawing code is the hot path, so this file is
// compiled for speed instead.
#pragma GCC optimize("O2")
// Screen driver over SPI with DMA for the ST7789 2.4" 240x320 panel (config.h), landscape 320x240.
//  * two framebuffers when RAM allows (the last frame is kept to see what changed)
//  * pixels go out through small staging buffers, byte-swapped on the way, so drawing the next
//    frame never waits for the SPI
//  * only what changed goes over SPI (dirty.h), so a calm screen costs almost nothing
#ifdef ARDUINO
#include "display.h"

#include <Arduino.h>

#include "config.h"
#include "dirty.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"

static int W = 320, H = 240;
static int pinDC = -1, pinBLK = -1;
static const int CHUNK_PX = 4096;  // pixels per DMA transaction (8 KB)
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
// one buffer only: hashes per tile of the last frame sent (dirty.h: dirtyTiles)
static uint32_t* tileHash = nullptr;

static void IRAM_ATTR preCb(spi_transaction_t* t) {
  gpio_set_level((gpio_num_t)pinDC, (int)(intptr_t)t->user);
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
  uint16_t x0 = x, x1 = x + w - 1, y0 = y, y1 = y + h - 1;
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

// ST7789 orientation (MADCTL) per rotation 0..3; odd = landscape. MV swaps the axes, MX / MY mirror.
static const uint8_t MAD_ST7789[4] = {0x00, 0x60, 0xC0, 0xA0};

bool display_init(const Board& b) {
  int rot = b.rotation & 3;
  W = (rot & 1) ? 320 : 240;
  H = (rot & 1) ? 240 : 320;
  pinDC = b.tftDc;
  pinBLK = b.tftBlk;
  pinMode(b.tftDc, OUTPUT);
  pinMode(b.tftRes, OUTPUT);
  digitalWrite(b.tftRes, HIGH);
  delay(5);
  digitalWrite(b.tftRes, LOW);
  delay(20);
  digitalWrite(b.tftRes, HIGH);
  delay(120);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = b.tftSda;
  bus.miso_io_num = -1;
  bus.sclk_io_num = b.tftScl;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = CHUNK_PX * 2;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t dc = {};
  dc.clock_speed_hz = b.spiMhz * 1000000;
  dc.mode = b.spiMode;
  dc.spics_io_num = b.tftCs;
  dc.queue_size = sizeof(trs) / sizeof(trs[0]);
  dc.pre_cb = preCb;
  if (spi_bus_add_device(SPI2_HOST, &dc, &dev) != ESP_OK) return false;

  uint8_t colmod = 0x55;  // 16-bit colour
  uint8_t madctl = MAD_ST7789[rot] | (b.bgr ? 0x08 : 0x00);
  cmd(0x01);  // software reset
  delay(150);
  cmd(0x11);  // sleep out
  delay(120);
  cmdData(0x3A, &colmod, 1);
  cmdData(0x36, &madctl, 1);
  cmd(b.invert ? 0x21 : 0x20);
  cmd(0x13);  // normal display mode

  tileHash = (uint32_t*)heap_caps_calloc(dirtyTileCount(W, H), 4, MALLOC_CAP_INTERNAL);
  if (!tileHash) return false;
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
int display_width() { return W; }
int display_height() { return H; }
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
    n = dirtyTiles(b, W, H, tileHash, rects, MAXRECTS);
  }
  // Self-healing: one band of rows also goes out every frame, round the screen (all of it every
  // H / 8 frames: half a second on 320x240 at 60 fps). Only changes are sent otherwise, so a pixel
  // garbled on the wire (long or breadboard wires, a fast SPI clock) would stay on the panel
  // until that spot changed again. Costs ~5 KB a frame, about 1 ms at 40 MHz, so it rides only on
  // light frames: a scroll or a page slide is not slowed down, the band goes on right after it.
  static int healY = 0;
  int px = 0;
  for (int i = 0; i < n; i++) px += rects[i].w * rects[i].h;
  if (!first && n < MAXRECTS && px < 8000) {
    int hh = H - healY < DIRTY_BAND ? H - healY : DIRTY_BAND;
    rects[n++] = {0, healY, W, hh};
    healY = healY + DIRTY_BAND >= H ? 0 : healY + DIRTY_BAND;
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
    ledcAttachChannel(pinBLK, 20000, 8, 0);
    attached = true;
  }
  float u = pct / 100.f;
  int duty = (int)(u * u * 255);
  if (pct <= 0) duty = 0;  // asleep: really dark
  else if (duty < 6) duty = 6;
  ledcWrite(pinBLK, duty);
}
#endif
