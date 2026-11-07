// sdl output: the projector picture in a window on a PC (testing without a projector).
// Timing follows the monitor's vsync as far as the desktop allows: good enough to see the
// picture, not to judge real-time behaviour (that is what the drm output on the Pi is for).
#include "out.h"

#ifndef HAVE_SDL
Out* out_sdl_open(int w, int h, int fullscreen) {
  (void)w, (void)h, (void)fullscreen;
  return 0;
}
#else
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  SDL_Window* win;
  SDL_Renderer* ren;
  SDL_Texture* tex;
  uint32_t* buf;
} Sdl;

static uint32_t* s_back(Out* o) { return ((Sdl*)o->priv)->buf; }

static int s_flip(Out* o, int64_t* shown) {
  Sdl* s = o->priv;
  SDL_Event e;
  while (SDL_PollEvent(&e))
    if (e.type == SDL_QUIT) return -1;
  SDL_UpdateTexture(s->tex, NULL, s->buf, o->pitch);
  SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
  SDL_RenderClear(s->ren);
  SDL_RenderCopy(s->ren, s->tex, NULL, NULL);
  SDL_RenderPresent(s->ren);  // waits for the monitor's vsync (PRESENTVSYNC)
  *shown = now_ns();
  return 0;
}

static void s_title(Out* o, const char* t) {
  char b[200];
  snprintf(b, sizeof b, "SpinProj: проектор  ·  %s", t);
  SDL_SetWindowTitle(((Sdl*)o->priv)->win, b);
}

static void s_close(Out* o) {
  Sdl* s = o->priv;
  SDL_DestroyTexture(s->tex);
  SDL_DestroyRenderer(s->ren);
  SDL_DestroyWindow(s->win);
  SDL_Quit();
  free(s->buf);
  free(s);
  free(o);
}

Out* out_sdl_open(int w, int h, int fullscreen) {
  if (SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "sdl: %s\n", SDL_GetError());
    return 0;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  int ww = w, wh = h;
  while (ww > 960) ww /= 2, wh /= 2;  // a window that fits next to the others
  Uint32 fl = SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  Sdl* s = calloc(1, sizeof *s);
  s->win = SDL_CreateWindow("SpinProj: проектор", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh, fl);
  if (!s->win) {
    fprintf(stderr, "sdl: %s\n", SDL_GetError());
    free(s);
    return 0;
  }
  s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!s->ren) s->ren = SDL_CreateRenderer(s->win, -1, 0);
  SDL_RenderSetLogicalSize(s->ren, w, h);
  s->tex = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  s->buf = calloc((size_t)w * h, 4);
  SDL_DisplayMode m;
  double hz = 60;
  if (!SDL_GetCurrentDisplayMode(0, &m) && m.refresh_rate > 0) hz = m.refresh_rate;
  Out* o = calloc(1, sizeof *o);
  *o = (Out){.name = "sdl", .w = w, .h = h, .hz = hz, .nbuf = 1, .pitch = w * 4,
             .back_ptr = s_back, .flip = s_flip, .title = s_title, .close = s_close, .priv = s};
  return o;
}
#endif
