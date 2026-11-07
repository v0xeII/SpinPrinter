// SpinProj outputs: where the frames go.
//   drm  - the projector on the Raspberry Pi (DRM/KMS page flip on every vsync)
//   sdl  - a window on a PC (for testing without a projector)
//   null - nothing on screen, a precise virtual 60 Hz clock (tests, CI)
#pragma once
#include <stdint.h>

typedef struct Out Out;
struct Out {
  const char* name;
  int w, h;          // pixels
  double hz;         // refresh rate
  int nbuf;          // how many back buffers take turns (the main loop skips refilling an unchanged one)
  int back;          // index of the buffer back_ptr() returns
  int pitch;         // bytes per row of a back buffer (XRGB8888)
  uint32_t* (*back_ptr)(Out* o);
  // Show the back buffer at the next vsync. Blocks until it is on screen and returns the time
  // of that vsync (CLOCK_MONOTONIC, ns). 0 = ok, -1 = the output is gone.
  int (*flip)(Out* o, int64_t* shown_ns);
  void (*title)(Out* o, const char* text);  // optional: window title (sdl)
  void (*close)(Out* o);
  void* priv;
};

int64_t now_ns(void);
Out* out_null_open(int w, int h, double hz);
Out* out_sdl_open(int w, int h, int fullscreen);  // NULL if built without SDL2
Out* out_drm_open(int w, int h, const char* card);  // NULL if built without libdrm or no display
