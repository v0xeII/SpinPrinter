// Arduino builds with -Os (small code). The drawing code is the hot path, so this file is
// compiled for speed instead.
#pragma GCC optimize("O2")
// Screen driver over SPI with DMA: ST7789, ILI9341, ST7735 and GC9A01, chosen at run time (hw.h).
// All four speak the same pixel protocol (CASET / RASET / RAMWR, big-endian RGB565); only the
// start-up sequence, the orientation bits and where the picture sits in the controller differ.
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

static int W = 320, H = 170;
static int pinDC = -1, pinBLK = -1;
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

// Start-up sequences: {command, number of data bytes (| DELAY: a pause in ms follows), data...},
// ended by END. Colour mode, orientation and inversion are sent afterwards, the same for all.
static const uint8_t DELAY = 0x80, END = 0xFF;  // (0xFF is never a command with 0x80+ data bytes)
static const uint8_t SEQ_ST7789[] = {0x11, DELAY, 120, END, END};  // sleep out
static const uint8_t SEQ_ILI9341[] = {
    0x11, DELAY, 120,
    0xC0, 1, 0x23,        // power control 1
    0xC1, 1, 0x10,        // power control 2
    0xC5, 2, 0x3E, 0x28,  // VCOM
    0xC7, 1, 0x86,
    0xB1, 2, 0x00, 0x18,  // frame rate 79 Hz
    0xB6, 3, 0x08, 0x82, 0x27,
    0xF2, 1, 0x00,        // no 3-gamma
    0x26, 1, 0x01,
    0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
    0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
    END, END};
static const uint8_t SEQ_ST7735[] = {
    0x11, DELAY, 120,
    0xB1, 3, 0x01, 0x2C, 0x2D,  // frame rate
    0xB2, 3, 0x01, 0x2C, 0x2D,
    0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,
    0xB4, 1, 0x07,              // no inversion between lines
    0xC0, 3, 0xA2, 0x02, 0x84,  // power
    0xC1, 1, 0xC5,
    0xC2, 2, 0x0A, 0x00,
    0xC3, 2, 0x8A, 0x2A,
    0xC4, 2, 0x8A, 0xEE,
    0xC5, 1, 0x0E,              // VCOM
    0xE0, 16, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,
    0xE1, 16, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,
    END, END};
static const uint8_t SEQ_GC9A01[] = {  // mostly undocumented registers, as every GC9A01 driver sends them
    0xEF, 0, 0xEB, 1, 0x14, 0xFE, 0, 0xEF, 0, 0xEB, 1, 0x14,
    0x84, 1, 0x40, 0x85, 1, 0xFF, 0x86, 1, 0xFF, 0x87, 1, 0xFF, 0x88, 1, 0x0A, 0x89, 1, 0x21,
    0x8A, 1, 0x00, 0x8B, 1, 0x80, 0x8C, 1, 0x01, 0x8D, 1, 0x01, 0x8E, 1, 0xFF, 0x8F, 1, 0xFF,
    0xB6, 2, 0x00, 0x00,
    0x90, 4, 0x08, 0x08, 0x08, 0x08,
    0xBD, 1, 0x06, 0xBC, 1, 0x00,
    0xFF, 3, 0x60, 0x01, 0x04,
    0xC3, 1, 0x13, 0xC4, 1, 0x13, 0xC9, 1, 0x22, 0xBE, 1, 0x11,
    0xE1, 2, 0x10, 0x0E,
    0xDF, 3, 0x21, 0x0C, 0x02,
    0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
    0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
    0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xED, 2, 0x1B, 0x0B,
    0xAE, 1, 0x77, 0xCD, 1, 0x63,
    0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03,
    0xE8, 1, 0x34,
    0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
    0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
    0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
    0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
    0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
    0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00,
    0x98, 2, 0x3E, 0x07,
    0x35, 0,                    // tearing line on
    0x11, DELAY, 120,           // sleep out, last on this chip
    END, END};

static void runSeq(const uint8_t* s) {
  // END END ends it: 0xFF alone is a real GC9A01 command (with 3 data bytes)
  while (!(s[0] == END && s[1] == END)) {
    uint8_t c = *s++, n = *s++;
    cmdData(c, s, n & 0x7F);
    s += n & 0x7F;
    if (n & DELAY) delay(*s++);
  }
}

// orientation (MADCTL) per rotation 0..3; odd = landscape. MV swaps the axes, MX / MY mirror.
static const uint8_t MAD_ST7789[4] = {0x00, 0x60, 0xC0, 0xA0};
static const uint8_t MAD_ILI9341[4] = {0x40, 0x20, 0x80, 0xE0};
static const uint8_t MAD_ST7735[4] = {0xC0, 0xA0, 0x00, 0x60};

bool display_init(const HwConfig& hw) {
  hw_screen_size(hw, W, H);
  int rot = hw_rotation(hw);
  pinDC = hw.dc;
  pinBLK = hw.blk;
  pinMode(hw.dc, OUTPUT);
  if (hw.res >= 0) {
    pinMode(hw.res, OUTPUT);
    digitalWrite(hw.res, HIGH);
    delay(5);
    digitalWrite(hw.res, LOW);
    delay(20);
    digitalWrite(hw.res, HIGH);
  }
  delay(120);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = hw.sda;
  bus.miso_io_num = -1;
  bus.sclk_io_num = hw.scl;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = CHUNK_PX * 2;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t dc = {};
  dc.clock_speed_hz = hw.spiMhz * 1000000;
  dc.mode = hw.spiMode;
  dc.spics_io_num = hw.cs;
  dc.queue_size = sizeof(trs) / sizeof(trs[0]);
  dc.pre_cb = preCb;
  if (spi_bus_add_device(SPI2_HOST, &dc, &dev) != ESP_OK) return false;

  // orientation + where the visible area sits in the controller's memory
  const uint8_t* mad = MAD_ST7789;
  const uint8_t* seq = SEQ_ST7789;
  uint8_t colmod = 0x55;  // 16-bit colour
  int ox[4] = {0, 0, 0, 0}, oy[4] = {0, 0, 0, 0};
  switch (hw.panel) {
    case PANEL_ST7789_170x320:  // 170 columns in the middle of 240
      ox[0] = ox[2] = 35;
      oy[1] = oy[3] = 35;
      break;
    case PANEL_ST7789_240x240:  // 240 rows at one end of 320
      oy[2] = 80;
      ox[3] = 80;
      break;
    case PANEL_ILI9341: mad = MAD_ILI9341; seq = SEQ_ILI9341; break;
    case PANEL_ST7735: mad = MAD_ST7735; seq = SEQ_ST7735; colmod = 0x05; break;
    case PANEL_GC9A01: seq = SEQ_GC9A01; break;
    default: break;  // ST7789 240x320: the whole memory
  }
  uint8_t madctl = mad[rot] | (hw_bgr(hw) ? 0x08 : 0x00);
  offX = hw.offX == HW_AUTO ? ox[rot] : hw.offX;
  offY = hw.offY == HW_AUTO ? oy[rot] : hw.offY;

  cmd(0x01);  // software reset
  delay(150);
  runSeq(seq);
  cmdData(0x3A, &colmod, 1);
  cmdData(0x36, &madctl, 1);
  cmd(hw_invert(hw) ? 0x21 : 0x20);
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
    if (pinBLK < 0) return;  // no backlight pin: always on
    ledcAttachChannel(pinBLK, 20000, 8, 0);
    attached = true;
  }
  float u = pct / 100.f;
  int duty = (int)(u * u * 255);
  if (duty < 6) duty = 6;
  ledcWrite(pinBLK, duty);
}
#endif
