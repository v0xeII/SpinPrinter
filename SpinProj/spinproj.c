// SpinProj: shows the frame that belongs to the vial's angle, on every vsync of the projector.
//
// The old way (OpenCAL, SpinPi 0.4 with mpv) plays a video by time and hopes the motor keeps up.
// Here the motor is the clock: the ESP32 counts every step, SpinPi forwards the angle 20 times a
// second, and at each vsync SpinProj predicts the angle for that moment and puts up the frame for
// it. The picture cannot drift, and a missed vsync does not shift anything: the next one shows the
// right frame again.
//
// One turn of frames is decoded before the print (ffmpeg: any video, image sequence or lossless
// file), kept in RAM as 8-bit grey compressed with LZ4, and unpacked into the back buffer right
// before each flip.
//
// Talks to SpinPi over stdin/stdout, one line per command (see README.md):
//   LOAD <frames per turn> <path> | IMAGE <file.pgm> | BLACK | OFFSET <px> | PHASE <deg>
//   A <angle deg> <rpm> <t ns> [round trip ns] | ARM <turns> | STOP | PING | QUIT
// and answers READY / LOADING / LOADED / LIGHT / STAT / DONE / STOPPED / ERR / INFO lines.
//
//   spinproj [--out drm|sdl|null] [--size 1280x800] [--rt] [--cpu 3] [--mem-mb N]
//   spinproj bench [--video file.mp4] [--frames 360] [--seconds 10] [--out ...]
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <lz4.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "out.h"

#define VERSION "0.1.0"
#define ANGLE_STALE_NS 300000000LL  // no angle for 0.3 s while the light is on: black at once
#define STAT_EVERY_NS 500000000LL

// ============================================================================ output lines
static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;
static void emit(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&out_mu);
  vfprintf(stdout, fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  pthread_mutex_unlock(&out_mu);
  va_end(ap);
}

// ============================================================================ frames in RAM
typedef struct {
  int n, w, h;
  uint8_t** data;
  int* len;  // compressed size; == w*h means stored as is
  size_t raw, comp;
} Frames;

static void frames_free(Frames* f) {
  if (!f) return;
  for (int i = 0; i < f->n; i++) free(f->data[i]);
  free(f->data);
  free(f->len);
  free(f);
}

static int frame_unpack(const Frames* f, int i, uint8_t* dst) {
  int sz = f->w * f->h;
  if (f->len[i] == sz) {
    memcpy(dst, f->data[i], sz);
    return 0;
  }
  return LZ4_decompress_safe((const char*)f->data[i], (char*)dst, f->len[i], sz) == sz ? 0 : -1;
}

static int frame_add(Frames* f, const uint8_t* gray) {
  int sz = f->w * f->h, cap = LZ4_compressBound(sz);
  char* tmp = malloc(cap);
  int c = LZ4_compress_default((const char*)gray, tmp, sz, cap);
  uint8_t* keep;
  if (c <= 0 || c >= sz) {
    keep = malloc(sz);
    memcpy(keep, gray, sz);
    c = sz;
  } else {
    keep = malloc(c);
    memcpy(keep, tmp, c);
  }
  free(tmp);
  f->data = realloc(f->data, sizeof(*f->data) * (f->n + 1));
  f->len = realloc(f->len, sizeof(*f->len) * (f->n + 1));
  f->data[f->n] = keep;
  f->len[f->n] = c;
  f->n++;
  f->raw += sz;
  f->comp += c;
  return 0;
}

static size_t mem_available(void) {
  FILE* m = fopen("/proc/meminfo", "r");
  char line[128];
  size_t kb = 0;
  while (m && fgets(line, sizeof line, m))
    if (sscanf(line, "MemAvailable: %zu kB", &kb) == 1) break;
  if (m) fclose(m);
  return kb * 1024;
}

// Decode with ffmpeg straight to w x h 8-bit grey (scaled to fit and centred if the video has
// another size). progress(i) is called after each frame. Returns NULL + message on failure.
static Frames* decode(const char* path, int want, int w, int h, size_t budget, void (*progress)(int, int),
                      char* err, size_t errn) {
  char vf[256];
  snprintf(vf, sizeof vf,
           "scale=%d:%d:force_original_aspect_ratio=decrease:out_range=full:flags=area,"
           "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,format=gray",
           w, h, w, h);
  char nfr[16];
  snprintf(nfr, sizeof nfr, "%d", want);
  int pfd[2];
  if (pipe(pfd)) {
    snprintf(err, errn, "pipe: %s", strerror(errno));
    return 0;
  }
  pid_t pid = fork();
  if (pid == 0) {
    dup2(pfd[1], 1);
    close(pfd[0]);
    close(pfd[1]);
    execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-i", path, "-frames:v", nfr, "-vf", vf, "-f", "rawvideo",
           "-pix_fmt", "gray", "-", (char*)0);
    _exit(127);
  }
  close(pfd[1]);
  Frames* f = calloc(1, sizeof *f);
  f->w = w;
  f->h = h;
  int sz = w * h;
  uint8_t* buf = malloc(sz);
  err[0] = 0;
  for (;;) {
    int got = 0;
    while (got < sz) {
      ssize_t r = read(pfd[0], buf + got, sz - got);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      got += (int)r;
    }
    if (got < sz) break;
    frame_add(f, buf);
    if (progress) progress(f->n, want);
    if (budget && f->comp > budget) {
      snprintf(err, errn, "не хватает памяти: %d кадров уже %.0f МБ", f->n, f->comp / 1e6);
      break;
    }
    if (f->n >= want) break;
  }
  free(buf);
  close(pfd[0]);
  kill(pid, SIGTERM);
  int st = 0;
  waitpid(pid, &st, 0);
  if (!err[0] && f->n == 0)
    snprintf(err, errn, WIFEXITED(st) && WEXITSTATUS(st) == 127 ? "ffmpeg не установлен" : "ffmpeg не смог прочитать файл");
  if (err[0]) {
    frames_free(f);
    return 0;
  }
  return f;
}

// binary PGM (P5, 8 bit), what SpinPi writes for the test patterns
static uint8_t* read_pgm(const char* path, int* w, int* h) {
  FILE* fp = fopen(path, "rb");
  if (!fp) return 0;
  int W = 0, H = 0, mx = 0;
  if (fscanf(fp, "P5 %d %d %d", &W, &H, &mx) != 3 || W <= 0 || H <= 0 || mx != 255) {
    fclose(fp);
    return 0;
  }
  fgetc(fp);
  uint8_t* p = malloc((size_t)W * H);
  size_t r = fread(p, 1, (size_t)W * H, fp);
  fclose(fp);
  if (r != (size_t)W * H) {
    free(p);
    return 0;
  }
  *w = W;
  *h = H;
  return p;
}

// grey w x h -> XRGB8888 output, centred, shifted right by off pixels, black around it
static uint32_t lut[256];
static void blit(Out* o, const uint8_t* g, int w, int h, int off) {
  uint32_t* dst = o->back_ptr(o);
  int stride = o->pitch / 4;
  int x0 = (o->w - w) / 2 + off, y0 = (o->h - h) / 2;
  int sx0 = x0 < 0 ? -x0 : 0, dx0 = x0 < 0 ? 0 : x0;
  int cw = w - sx0;
  if (dx0 + cw > o->w) cw = o->w - dx0;
  for (int y = 0; y < o->h; y++) {
    uint32_t* d = dst + (size_t)y * stride;
    int sy = y - y0;
    if (sy < 0 || sy >= h || cw <= 0) {
      memset(d, 0, (size_t)o->w * 4);
      continue;
    }
    const uint8_t* s = g + (size_t)sy * w + sx0;
    if (dx0) memset(d, 0, (size_t)dx0 * 4);
    uint32_t* dd = d + dx0;
    for (int x = 0; x < cw; x++) dd[x] = lut[s[x]];
    if (dx0 + cw < o->w) memset(d + dx0 + cw, 0, (size_t)(o->w - dx0 - cw) * 4);
  }
}

static void blit_black(Out* o) {
  uint32_t* dst = o->back_ptr(o);
  for (int y = 0; y < o->h; y++) memset((uint8_t*)dst + (size_t)y * o->pitch, 0, (size_t)o->w * 4);
}

// ============================================================================ shared state
enum { M_BLACK, M_IMAGE, M_ARMED, M_LIGHT };
typedef struct {
  int have;
  double a;     // angle at t (deg, as the ESP counts it)
  int64_t t;    // ns
  double rate;  // deg/s, signed
  int dir;      // +1 / -1: which way the angle counts while turning forward
  double pa;    // previous raw sample, to learn the direction
  int64_t pt;
  int64_t rx;   // when the last sample arrived
  double resid_ms;  // how far samples land from the prediction, in time
  double rtt_min;   // shortest recent round trip of a POS query (ns): those samples are the most exact
  int off;          // samples in a row that disagree with the prediction by more than 1°
} Model;

static struct {
  pthread_mutex_t mu;
  int mode;
  Frames* fr;
  int fr_id;
  uint8_t* img;
  int img_w, img_h, img_id;
  int offset;
  double phase;  // forward angle that shows frame 0
  Model m;
  double turns;  // ARM
  int loading;
  int quit;
} S = {.mu = PTHREAD_MUTEX_INITIALIZER, .m = {.dir = 1}};
static pthread_mutex_t fill_mu;  // held while a frame is unpacked from S.fr (frames are swapped under it)
static int proj_w = 1280, proj_h = 800;
static size_t mem_budget = 0;

static double predict(const Model* m, int64_t t) { return m->a + m->rate * (double)(t - m->t) / 1e9; }

// The motor runs off a hardware timer and the ESP reports its exact speed, so the angle is a
// straight line; only the time stamp of each sample is noisy (USB + scheduling, the middle of the
// POS round trip). So: speed from the ESP, phase corrected slowly, and quick answers trusted most.
static void model_sample(Model* m, double a, double rpm, int64_t t, double rtt, int64_t rx) {
  double speed = fabs(rpm) * 6.0;
  if (m->have && t > m->pt && speed > 0.5) {
    double v = (a - m->pa) / ((double)(t - m->pt) / 1e9);
    if (v < -0.5 * speed) m->dir = -1;
    else if (v > 0.5 * speed) m->dir = 1;
  }
  m->pa = a;
  m->pt = t;
  m->rate = m->dir * speed;
  if (!m->have || rx - m->rx > ANGLE_STALE_NS || fabs(a - predict(m, t)) > 5.0) {
    m->a = a;  // first sample, a gap, or a jump (angle reset): take it as it is
    m->resid_ms = 0;
  } else {
    double e = a - predict(m, t);
    double gain = 0.08;
    m->off = fabs(e) > 1.0 ? m->off + 1 : 0;
    if (m->off >= 3) gain = 0.5;  // off again and again (the speed really changed): catch up fast
    if (rtt > 0) {
      m->rtt_min = m->rtt_min > 0 ? fmin(m->rtt_min * 1.002, rtt) : rtt;  // slowly forgets an old best
      if (rtt > 1.5 * m->rtt_min + 1e6) gain = 0.01;  // a slow answer: its time stamp is vague
    }
    m->a = predict(m, t) + gain * e;
    if (speed > 0.5) m->resid_ms = m->resid_ms * 0.9 + 0.1 * fabs(e) / speed * 1000.0;
  }
  m->t = t;
  m->rx = rx;
  m->have = 1;
}

// ============================================================================ commands (stdin)
static void load_progress(int i, int n) {
  if (i % 30 == 0 || i == n) emit("LOADING %d %d", i, n);
}

typedef struct { int n; char path[1024]; } LoadJob;
static void* loader(void* arg) {
  LoadJob* j = arg;
  char err[200];
  int64_t t0 = now_ns();
  size_t budget = mem_budget ? mem_budget : mem_available() * 3 / 4;
  Frames* f = decode(j->path, j->n, proj_w, proj_h, budget, load_progress, err, sizeof err);
  if (!f) {
    emit("ERR %s", err);
  } else {
    if (f->n < j->n) emit("INFO в файле только %d кадров из %d", f->n, j->n);
    pthread_mutex_lock(&fill_mu);
    pthread_mutex_lock(&S.mu);
    Frames* old = S.fr;
    S.fr = f;
    S.fr_id++;
    pthread_mutex_unlock(&S.mu);
    pthread_mutex_unlock(&fill_mu);
    frames_free(old);
    emit("LOADED %d %d %d %.1f %.1f %.2f", f->n, f->w, f->h, f->raw / 1e6, f->comp / 1e6, (now_ns() - t0) / 1e9);
  }
  pthread_mutex_lock(&S.mu);
  S.loading = 0;
  pthread_mutex_unlock(&S.mu);
  free(j);
  return 0;
}

static void command(char* line) {
  char* nl = strpbrk(line, "\r\n");
  if (nl) *nl = 0;
  int64_t rx = now_ns();
  if (line[0] == 'A' && line[1] == ' ') {
    double a, rpm, rtt = 0;
    long long t;
    if (sscanf(line + 2, "%lf %lf %lld %lf", &a, &rpm, &t, &rtt) >= 3) {
      pthread_mutex_lock(&S.mu);
      model_sample(&S.m, a, rpm, (int64_t)t, rtt, rx);
      pthread_mutex_unlock(&S.mu);
    }
    return;
  }
  pthread_mutex_lock(&S.mu);
  int busy = S.mode == M_ARMED || S.mode == M_LIGHT;
  if (!strcmp(line, "PING")) {
    emit("PONG SpinProj " VERSION);
  } else if (!strcmp(line, "BLACK")) {
    S.mode = M_BLACK;
  } else if (!strcmp(line, "STOP")) {
    if (busy) emit("STOPPED");
    S.mode = M_BLACK;
  } else if (!strcmp(line, "QUIT")) {
    S.mode = M_BLACK;
    S.quit = 1;
  } else if (!strncmp(line, "OFFSET ", 7)) {
    S.offset = atoi(line + 7);
  } else if (!strncmp(line, "PHASE ", 6)) {
    S.phase = atof(line + 6);
  } else if (!strncmp(line, "ARM ", 4)) {
    double t = atof(line + 4);
    if (!S.fr) emit("ERR нет кадров (сначала LOAD)");
    else if (S.loading) emit("ERR кадры ещё грузятся");
    else if (t <= 0) emit("ERR ARM: число оборотов");
    else if (!S.m.have || rx - S.m.rx > ANGLE_STALE_NS) emit("ERR нет угла от ESP (A …)");
    else {
      S.turns = t;
      S.mode = M_ARMED;
    }
  } else if (!strncmp(line, "LOAD ", 5)) {
    int n = 0, used = 0;
    if (busy) emit("ERR идёт печать");
    else if (S.loading) emit("ERR уже гружу");
    else if (sscanf(line + 5, "%d %n", &n, &used) < 1 || n <= 0 || !line[5 + used]) emit("ERR LOAD <кадров> <файл>");
    else {
      LoadJob* j = calloc(1, sizeof *j);
      j->n = n;
      snprintf(j->path, sizeof j->path, "%s", line + 5 + used);
      S.loading = 1;
      pthread_t th;
      pthread_create(&th, 0, loader, j);
      pthread_detach(th);
    }
  } else if (!strncmp(line, "IMAGE ", 6)) {
    int w, h;
    uint8_t* p = busy ? 0 : read_pgm(line + 6, &w, &h);
    if (busy) emit("ERR идёт печать");
    else if (!p) emit("ERR не читается картинка %s", line + 6);
    else {
      free(S.img);
      S.img = p;
      S.img_w = w;
      S.img_h = h;
      S.img_id++;
      S.mode = M_IMAGE;
    }
  } else if (line[0]) {
    emit("ERR непонятная команда: %.60s", line);
  }
  pthread_mutex_unlock(&S.mu);
}

static void* reader(void* arg) {
  (void)arg;
  // plain read(), not stdio: a thread blocked in fgets(stdin) holds the stdin lock, and exit()
  // would wait for it forever while flushing
  char buf[4096];
  int n = 0;
  for (;;) {
    ssize_t r = read(0, buf + n, sizeof buf - 1 - n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    n += (int)r;
    char* s = buf;
    for (char* e; (e = memchr(s, '\n', buf + n - s));) {
      *e = 0;
      command(s);
      s = e + 1;
    }
    n -= (int)(s - buf);
    memmove(buf, s, n);
    if (n >= (int)sizeof buf - 1) n = 0;  // a line that long is garbage
  }
  pthread_mutex_lock(&S.mu);  // SpinPi is gone: never leave the light on
  S.mode = M_BLACK;
  S.quit = 1;
  pthread_mutex_unlock(&S.mu);
  return 0;
}

// ============================================================================ real time
static void realtime(int cpu) {
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) fprintf(stderr, "rt: mlockall: %s\n", strerror(errno));
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set))
      fprintf(stderr, "rt: cannot pin to CPU %d\n", cpu);
  }
  struct sched_param sp = {.sched_priority = 80};
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
    fprintf(stderr, "rt: SCHED_FIFO not allowed (run as root)\n");
}

// ============================================================================ the vsync loop
typedef struct { int kind, id, idx, off; } Content;  // what a back buffer holds
enum { C_NONE, C_BLACK, C_IMAGE, C_FRAME };

static int run(Out* o) {
  int64_t period = (int64_t)(1e9 / o->hz + 0.5);
  Content have[4] = {{0}};
  Content shown = {.kind = C_BLACK};  // what is on screen now
  uint8_t* gray = malloc((size_t)proj_w * proj_h);
  int64_t t_prev = now_ns();
  // exposure bookkeeping
  double start = 0, end = 0;
  int dir = 1;
  int nfr = 0;
  long* dose = 0;  // refreshes per frame index
  long flips = 0, missed = 0;
  double fill_max = 0, period_avg = (double)period;
  int64_t last_stat = 0;
  int ended = 0;  // the last turn is complete: report DONE once the dark frame is up
  char title[160] = "";
  for (;;) {
    int64_t t_next = t_prev + (int64_t)period_avg;
    Content want = {.kind = C_BLACK};
    pthread_mutex_lock(&S.mu);
    if (S.quit && S.mode == M_BLACK && shown.kind == C_BLACK) {
      pthread_mutex_unlock(&S.mu);
      break;
    }
    int64_t now = now_ns();
    if ((S.mode == M_ARMED || S.mode == M_LIGHT) && now - S.m.rx > ANGLE_STALE_NS) {
      S.mode = M_BLACK;
      emit("ERR угол от ESP не приходит %.0f мс: свет выключен", (now - S.m.rx) / 1e6);
    }
    if (S.mode == M_ARMED) {  // light on from this vsync for exactly S.turns turns
      dir = S.m.dir;
      start = dir * predict(&S.m, t_next);
      end = start + S.turns * 360.0;
      nfr = S.fr->n;
      free(dose);
      dose = calloc(nfr, sizeof *dose);
      flips = missed = 0;
      fill_max = 0;
      S.mode = M_LIGHT;
      emit("LIGHT %.4f", dir * start);
    }
    if (S.mode == M_LIGHT) {
      double f = dir * predict(&S.m, t_next);
      if (f >= end) {
        S.mode = M_BLACK;  // the turn boundary: this vsync is already dark
        ended = 1;
      } else {
        double u = fmod(f - S.phase, 360.0);
        if (u < 0) u += 360.0;
        int idx = (int)(u / 360.0 * nfr);
        if (idx >= nfr) idx = nfr - 1;
        want = (Content){C_FRAME, S.fr_id, idx, S.offset};
      }
      if (now - last_stat > STAT_EVERY_NS) {
        last_stat = now;
        emit("STAT %.2f %ld %ld %.2f %.2f", fmin(f, end) - start, flips, missed, fill_max, S.m.resid_ms);
        if (o->title) {
          snprintf(title, sizeof title, "свет · кадр %d/%d · оборот %.2f из %.0f · пропущено vsync: %ld",
                   want.kind == C_FRAME ? want.idx + 1 : 0, nfr, (fmin(f, end) - start) / 360.0, S.turns, missed);
          o->title(o, title);
        }
      }
    } else if (S.mode == M_IMAGE && S.img) {
      want = (Content){C_IMAGE, S.img_id, 0, 0};
    }
    pthread_mutex_unlock(&S.mu);

    // fill the back buffer unless it already holds this picture
    Content* hb = &have[o->back];
    if (hb->kind != want.kind || hb->id != want.id || hb->idx != want.idx || hb->off != want.off) {
      int64_t f0 = now_ns();
      if (want.kind == C_FRAME) {
        pthread_mutex_lock(&fill_mu);
        int ok = S.fr && S.fr_id == want.id && !frame_unpack(S.fr, want.idx, gray);
        if (ok) blit(o, gray, S.fr->w, S.fr->h, want.off);
        pthread_mutex_unlock(&fill_mu);
        if (!ok) want.kind = C_BLACK, blit_black(o);
      } else if (want.kind == C_IMAGE) {
        pthread_mutex_lock(&S.mu);
        blit(o, S.img, S.img_w, S.img_h, 0);
        pthread_mutex_unlock(&S.mu);
      } else {
        blit_black(o);
      }
      double ms = (now_ns() - f0) / 1e6;
      if (want.kind == C_FRAME && ms > fill_max) fill_max = ms;
      *hb = want;
    }
    int64_t t_shown;
    if (o->flip(o, &t_shown)) {
      emit("ERR вывод на проектор пропал");
      break;
    }
    // how many refreshes the previous picture stayed up (more than 1 = missed vsync)
    int gap = (int)llround((double)(t_shown - t_prev) / period_avg);
    if (gap < 1) gap = 1;
    if (gap == 1) period_avg += ((double)(t_shown - t_prev) - period_avg) * 0.01;
    if (shown.kind == C_FRAME && dose && shown.idx < nfr) {
      dose[shown.idx] += gap;
      flips++;
      missed += gap - 1;
    }
    if (shown.kind == C_FRAME && want.kind != C_FRAME) {  // the light just went off
      long lo = dose ? dose[0] : 0, hi = lo, sum = 0;
      for (int i = 0; dose && i < nfr; i++) {
        if (dose[i] < lo) lo = dose[i];
        if (dose[i] > hi) hi = dose[i];
        sum += dose[i];
      }
      if (ended) emit("DONE %ld %ld %ld %ld %.2f %.2f", flips, missed, lo, hi, nfr ? (double)sum / nfr : 0.0, fill_max);
      if (o->title) o->title(o, ended ? "готово, темно" : "темно");
      ended = 0;
    }
    shown = want;
    t_prev = t_shown;
  }
  free(gray);
  free(dose);
  return 0;
}

// ============================================================================ bench
// CAL-like test frames: smooth grey projections of a few rotating blobs (not real slicer output)
static void synth_frame(uint8_t* g, int w, int h, int k, int n) {
  double th = 2 * M_PI * k / n;
  struct { double r, z, rad, hz; } b[] = {{0.0, 0.0, 0.22, 0.30}, {0.18, 0.15, 0.10, 0.12}, {0.25, -0.2, 0.08, 0.10}};
  int vial = h * 9 / 10;  // vial diameter on the projector, px
  for (int y = 0; y < h; y++) {
    double zy = (y - h / 2.0) / vial;
    for (int x = 0; x < w; x++) {
      double xx = (x - w / 2.0) / vial, v = 0;
      for (int i = 0; i < 3; i++) {
        double cx = b[i].r * cos(th + i * 2.1);
        double dz = (zy - b[i].z) / b[i].hz;
        if (fabs(dz) >= 1) continue;
        double rr = b[i].rad * sqrt(1 - dz * dz), dx = xx - cx;
        if (fabs(dx) < rr) v += sqrt(rr * rr - dx * dx);
      }
      double val = v * 900;
      g[(size_t)y * w + x] = (uint8_t)(val > 255 ? 255 : val);
    }
  }
}

static int cmp_d(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : x > y;
}

static int bench(Out* o, const char* video, int n, double seconds) {
  int w = proj_w, h = proj_h, sz = w * h;
  printf("== SpinProj %s bench: вывод %s %dx%d %.3f Гц, кадры %dx%d\n", VERSION, o->name, o->w, o->h, o->hz, w, h);
  Frames* f;
  uint8_t* g = malloc(sz);
  int64_t t0 = now_ns();
  if (video) {
    char err[200];
    f = decode(video, n, w, h, 0, 0, err, sizeof err);
    if (!f) {
      printf("ошибка: %s\n", err);
      return 1;
    }
    printf("видео %s: %d кадров, разжато ffmpeg за %.1f с (%.1f кадр/с)\n", video, f->n, (now_ns() - t0) / 1e9,
           f->n / ((now_ns() - t0) / 1e9));
  } else {
    f = calloc(1, sizeof *f);
    f->w = w;
    f->h = h;
    double tc = 0;
    for (int k = 0; k < n; k++) {
      synth_frame(g, w, h, k, n);
      int64_t c0 = now_ns();
      frame_add(f, g);
      tc += (now_ns() - c0) / 1e6;
    }
    printf("синтетические кадры (похожи на CAL, но не настоящие): %d шт, сжатие LZ4 %.2f мс/кадр\n", n, tc / n);
  }
  n = f->n;
  printf("память: без сжатия %.0f МБ, LZ4 %.1f МБ (в %.1f раз меньше), %.0f КБ/кадр\n", f->raw / 1e6, f->comp / 1e6,
         (double)f->raw / f->comp, f->comp / 1e3 / n);
  size_t avail = mem_available();
  printf("свободно RAM: %.0f МБ -> влезает примерно %.0f таких кадров (75%% свободной)\n", avail / 1e6,
         avail * 0.75 / ((double)f->comp / n));
  // unpack speed
  double* ts = malloc(sizeof(double) * n * 3);
  for (int r = 0; r < 3; r++)
    for (int i = 0; i < n; i++) {
      int64_t a = now_ns();
      frame_unpack(f, i, g);
      ts[r * n + i] = (now_ns() - a) / 1e6;
    }
  qsort(ts, n * 3, sizeof *ts, cmp_d);
  printf("распаковка кадра: медиана %.2f мс, 99%% %.2f мс, макс %.2f мс\n", ts[n * 3 / 2], ts[n * 3 * 99 / 100],
         ts[n * 3 - 1]);
  double unpack_med = ts[n * 3 / 2];
  // fill (unpack + convert into the output buffer) on every vsync
  int64_t period = (int64_t)(1e9 / o->hz + 0.5);
  long cap = (long)(seconds * o->hz) + 2;
  double* iv = malloc(sizeof(double) * cap);
  double* fl = malloc(sizeof(double) * cap);
  long cnt = 0, miss = 0;
  int64_t tp = 0, tq;
  o->flip(o, &tp);
  int64_t tend = tp + (int64_t)(seconds * 1e9);
  while (cnt < cap - 1) {
    int64_t a = now_ns();
    frame_unpack(f, (int)(cnt % n), g);
    blit(o, g, w, h, 0);
    fl[cnt] = (now_ns() - a) / 1e6;
    if (o->flip(o, &tq)) break;
    iv[cnt] = (tq - tp) / 1e6;
    miss += llround((double)(tq - tp) / period) - 1;
    tp = tq;
    cnt++;
    if (tq >= tend) break;
  }
  double sum = 0, sq = 0;
  for (long i = 0; i < cnt; i++) sum += iv[i], sq += iv[i] * iv[i];
  double mean = sum / cnt, sd = sqrt(fmax(0, sq / cnt - mean * mean));
  qsort(iv, cnt, sizeof *iv, cmp_d);
  qsort(fl, cnt, sizeof *fl, cmp_d);
  printf("vsync за %.0f с: %ld кадров, интервал средний %.3f мс (±%.3f), мин %.3f, макс %.3f, пропущено %ld\n", seconds,
         cnt, mean, sd, iv[0], iv[cnt - 1], miss);
  printf("подготовка кадра (распаковка + в буфер): медиана %.2f мс, макс %.2f мс, бюджет %.2f мс\n", fl[cnt / 2],
         fl[cnt - 1], 1e3 / o->hz);
  if (!strcmp(o->name, "sdl")) printf("(окно на ПК: vsync рабочего стола, для Pi эти цифры ничего не значат)\n");
  printf("RESULT out=%s w=%d h=%d hz=%.3f frames=%d raw_mb=%.1f lz4_mb=%.1f ratio=%.2f unpack_ms=%.3f "
         "fill_ms_max=%.3f vsync_mean_ms=%.3f vsync_sd_ms=%.3f vsync_max_ms=%.3f missed=%ld mem_avail_mb=%.0f\n",
         o->name, w, h, o->hz, n, f->raw / 1e6, f->comp / 1e6, (double)f->raw / f->comp, unpack_med, fl[cnt - 1], mean,
         sd, iv[cnt - 1], miss, avail / 1e6);
  free(ts);
  free(iv);
  free(fl);
  free(g);
  frames_free(f);
  return 0;
}

// ============================================================================ main
int main(int argc, char** argv) {
  const char *out = "drm", *card = "", *video = 0;
  int rt = 0, cpu = -1, fullscreen = 0, is_bench = 0, frames = 360;
  double hz = 60, seconds = 10;
  char name[64];
  for (int i = 1; i < argc; i++) {
    // "--opt value" and "--opt=value" both work
    const char* a = argv[i];
    const char* eq = strncmp(a, "--", 2) ? 0 : strchr(a, '=');
    const char* v = eq ? eq + 1 : (i + 1 < argc ? argv[i + 1] : "");
    if (eq) {
      snprintf(name, sizeof name, "%.*s", (int)(eq - a), a);
      a = name;
    }
#define OPT(n) (!strcmp(a, n) && (eq || ++i))
    if (!strcmp(a, "bench")) is_bench = 1;
    else if (OPT("--out")) out = v;
    else if (OPT("--size")) {
      if (!strcmp(v, "auto")) proj_w = proj_h = 0;  // the projector's own mode (drm)
      else sscanf(v, "%dx%d", &proj_w, &proj_h);
    }
    else if (OPT("--card")) card = v;
    else if (OPT("--hz")) hz = atof(v);
    else if (OPT("--cpu")) cpu = atoi(v);
    else if (OPT("--mem-mb")) mem_budget = (size_t)atol(v) << 20;
    else if (OPT("--video")) video = v;
    else if (OPT("--frames")) frames = atoi(v);
    else if (OPT("--seconds")) seconds = atof(v);
    else if (!strcmp(a, "--rt")) rt = 1;
    else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
    else if (!strcmp(a, "--version")) { puts("SpinProj " VERSION); return 0; }
    else {
      fprintf(stderr, "usage: spinproj [bench] [--out drm|sdl|null] [--size 1280x800|auto] [--rt] [--cpu N]\n"
                      "       [--mem-mb N] [--hz 60 (null)] [--fullscreen (sdl)] [--card /dev/dri/cardN]\n"
                      "       bench: [--video file] [--frames 360] [--seconds 10]\n");
      return 2;
    }
  }
  for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | (uint32_t)i * 0x010101u;
  signal(SIGPIPE, SIG_IGN);
  if ((!proj_w || !proj_h) && strcmp(out, "drm")) proj_w = 1280, proj_h = 800;
  Out* o = !strcmp(out, "null") ? out_null_open(proj_w, proj_h, hz)
           : !strcmp(out, "sdl") ? out_sdl_open(proj_w, proj_h, fullscreen)
                                 : out_drm_open(proj_w, proj_h, card);
  if (!o) {
    emit("ERR нет вывода «%s»%s", out, !strcmp(out, "sdl") ? " (собрано без SDL2?)" : "");
    return 1;
  }
  if (!proj_w || !proj_h) proj_w = o->w, proj_h = o->h;
  if (is_bench) {
    if (rt) realtime(cpu);
    int r = bench(o, video, frames, seconds);
    o->close(o);
    return r;
  }
  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
  pthread_mutex_init(&fill_mu, &ma);
  pthread_mutex_init(&S.mu, &ma);
  pthread_t th;
  pthread_create(&th, 0, reader, 0);  // created first: it and the loaders stay normal priority
  if (rt) realtime(cpu);
  emit("READY %s %d %d %.3f", o->name, o->w, o->h, o->hz);
  run(o);
  o->close(o);
  return 0;
}
