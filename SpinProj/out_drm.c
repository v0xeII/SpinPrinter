// drm output: the projector on HDMI, straight through DRM/KMS (no desktop, no X, no Wayland).
// Two "dumb" buffers take turns: while one is on screen the next frame is written into the
// other, then a page flip swaps them exactly at the vsync. The flip event carries the
// time of that vsync, so the main loop knows when each frame really appeared.
#include "out.h"

#ifndef HAVE_DRM
Out* out_drm_open(int w, int h, const char* card) {
  (void)w, (void)h, (void)card;
  return 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct {
  int fd;
  uint32_t crtc, conn;
  drmModeModeInfo mode;
  struct { uint32_t handle, fb; uint8_t* map; uint64_t size; } b[2];
  int on_screen;  // which buffer is being scanned out
  int monotonic;  // flip timestamps are CLOCK_MONOTONIC
  volatile int flipped;
  int64_t flip_ns;
} Drm;

static uint32_t* d_back(Out* o) {
  Drm* d = o->priv;
  return (uint32_t*)d->b[o->back].map;
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void* data) {
  (void)fd, (void)seq;
  Drm* d = data;
  d->flip_ns = (int64_t)sec * 1000000000LL + (int64_t)usec * 1000;
  d->flipped = 1;
}

static int d_flip(Out* o, int64_t* shown) {
  Drm* d = o->priv;
  d->flipped = 0;
  if (drmModePageFlip(d->fd, d->crtc, d->b[o->back].fb, DRM_MODE_PAGE_FLIP_EVENT, d)) {
    fprintf(stderr, "drm: page flip: %s\n", strerror(errno));
    return -1;
  }
  drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
  while (!d->flipped) {
    struct pollfd p = {.fd = d->fd, .events = POLLIN};
    int r = poll(&p, 1, 500);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) {
      fprintf(stderr, "drm: no vsync for 500 ms (HDMI unplugged?)\n");
      return -1;
    }
    drmHandleEvent(d->fd, &ev);
  }
  *shown = d->monotonic ? d->flip_ns : now_ns();
  d->on_screen = o->back;
  o->back ^= 1;
  return 0;
}

static void d_close(Out* o) {
  Drm* d = o->priv;
  // leave the projector black: clear what is on screen instead of restoring a console
  memset(d->b[d->on_screen].map, 0, d->b[d->on_screen].size);
  for (int i = 0; i < 2; i++) {
    munmap(d->b[i].map, d->b[i].size);
    drmModeRmFB(d->fd, d->b[i].fb);
    struct drm_mode_destroy_dumb dd = {.handle = d->b[i].handle};
    drmIoctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
  }
  close(d->fd);
  free(d);
  free(o);
}

static int make_buffer(Drm* d, int i, int w, int h, int* pitch) {
  struct drm_mode_create_dumb c = {.width = (uint32_t)w, .height = (uint32_t)h, .bpp = 32};
  if (drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return -1;
  d->b[i].handle = c.handle;
  d->b[i].size = c.size;
  *pitch = (int)c.pitch;
  if (drmModeAddFB(d->fd, (uint32_t)w, (uint32_t)h, 24, 32, c.pitch, c.handle, &d->b[i].fb)) return -1;
  struct drm_mode_map_dumb m = {.handle = c.handle};
  if (drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return -1;
  d->b[i].map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)m.offset);
  if (d->b[i].map == MAP_FAILED) return -1;
  memset(d->b[i].map, 0, c.size);
  return 0;
}

// first connected connector (HDMI preferred), its mode closest to w x h, and a CRTC for it
static int pick(Drm* d, int w, int h) {
  drmModeRes* res = drmModeGetResources(d->fd);
  if (!res) return -1;
  int found = 0;
  for (int pass = 0; pass < 2 && !found; pass++)
    for (int i = 0; i < res->count_connectors && !found; i++) {
      drmModeConnector* c = drmModeGetConnector(d->fd, res->connectors[i]);
      if (!c) continue;
      int hdmi = c->connector_type == DRM_MODE_CONNECTOR_HDMIA || c->connector_type == DRM_MODE_CONNECTOR_HDMIB;
      if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && (hdmi || pass == 1)) {
        int best = 0;
        for (int m = 0; m < c->count_modes; m++) {
          drmModeModeInfo* mi = &c->modes[m];
          if (mi->hdisplay == w && mi->vdisplay == h) { best = m; break; }
          if (mi->type & DRM_MODE_TYPE_PREFERRED) best = m;
        }
        d->mode = c->modes[best];
        d->conn = c->connector_id;
        drmModeEncoder* e = c->encoder_id ? drmModeGetEncoder(d->fd, c->encoder_id) : 0;
        if (e && e->crtc_id) d->crtc = e->crtc_id;
        for (int k = 0; !d->crtc && k < c->count_encoders; k++) {
          drmModeEncoder* e2 = drmModeGetEncoder(d->fd, c->encoders[k]);
          for (int j = 0; e2 && j < res->count_crtcs; j++)
            if (e2->possible_crtcs & (1u << j)) { d->crtc = res->crtcs[j]; break; }
          drmModeFreeEncoder(e2);
        }
        drmModeFreeEncoder(e);
        found = d->crtc != 0;
      }
      drmModeFreeConnector(c);
    }
  drmModeFreeResources(res);
  return found ? 0 : -1;
}

Out* out_drm_open(int w, int h, const char* card) {
  Drm* d = calloc(1, sizeof *d);
  char path[64];
  d->fd = -1;
  for (int i = 0; i < 4 && d->fd < 0; i++) {  // vc4 is usually card0 or card1
    snprintf(path, sizeof path, "/dev/dri/card%d", i);
    if (card && *card) snprintf(path, sizeof path, "%s", card);
    d->fd = open(path, O_RDWR | O_CLOEXEC);
    if (d->fd >= 0 && pick(d, w, h)) {
      close(d->fd);
      d->fd = -1;
    }
    if (card && *card) break;
  }
  if (d->fd < 0) {
    fprintf(stderr, "drm: no connected display found\n");
    free(d);
    return 0;
  }
  uint64_t cap = 0;
  d->monotonic = !drmGetCap(d->fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) && cap;
  int W = d->mode.hdisplay, H = d->mode.vdisplay, pitch = 0;
  if (make_buffer(d, 0, W, H, &pitch) || make_buffer(d, 1, W, H, &pitch)) {
    fprintf(stderr, "drm: cannot make buffers: %s\n", strerror(errno));
    free(d);
    return 0;
  }
  if (drmModeSetCrtc(d->fd, d->crtc, d->b[0].fb, 0, 0, &d->conn, 1, &d->mode)) {
    fprintf(stderr, "drm: set mode: %s (another program owns the display?)\n", strerror(errno));
    free(d);
    return 0;
  }
  double hz = d->mode.clock * 1000.0 / ((double)d->mode.htotal * d->mode.vtotal);
  if (d->mode.flags & DRM_MODE_FLAG_INTERLACE) hz *= 2;
  if (d->mode.vscan > 1) hz /= d->mode.vscan;
  Out* o = calloc(1, sizeof *o);
  *o = (Out){.name = "drm", .w = W, .h = H, .hz = hz, .nbuf = 2, .back = 1, .pitch = pitch,
             .back_ptr = d_back, .flip = d_flip, .close = d_close, .priv = d};
  fprintf(stderr, "drm: %s %dx%d %.3f Hz%s\n", path, W, H, hz, d->monotonic ? "" : " (no monotonic flip timestamps)");
  return o;
}
#endif
