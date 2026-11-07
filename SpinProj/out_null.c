// null output: no picture, an exact virtual vsync clock. For tests and for CI.
#include <stdlib.h>
#include <time.h>

#include "out.h"

int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

typedef struct {
  uint32_t* buf;
  int64_t next;  // next virtual vsync
  int64_t period;
} Null;

static uint32_t* n_back(Out* o) { return ((Null*)o->priv)->buf; }

static int n_flip(Out* o, int64_t* shown) {
  Null* n = o->priv;
  int64_t t = now_ns();
  if (n->next == 0) n->next = t + n->period;
  while (n->next < t) n->next += n->period;  // too late for this one: it is shown at the next vsync
  struct timespec ts = {(time_t)(n->next / 1000000000LL), (long)(n->next % 1000000000LL)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL)) {
  }
  *shown = n->next;
  n->next += n->period;
  return 0;
}

static void n_close(Out* o) {
  free(((Null*)o->priv)->buf);
  free(o->priv);
  free(o);
}

Out* out_null_open(int w, int h, double hz) {
  Out* o = calloc(1, sizeof *o);
  Null* n = calloc(1, sizeof *n);
  n->buf = calloc((size_t)w * h, 4);
  n->period = (int64_t)(1e9 / hz + 0.5);
  *o = (Out){.name = "null", .w = w, .h = h, .hz = hz, .nbuf = 1, .pitch = w * 4,
             .back_ptr = n_back, .flip = n_flip, .close = n_close, .priv = n};
  return o;
}
