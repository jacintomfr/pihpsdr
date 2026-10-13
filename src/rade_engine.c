/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* RADE V1 engine.  C port of the hpsdr-rs Rust worker (rade/worker.rs,
* rade/mod.rs, rade/vocoder.rs, rade_link.rs), itself adapted from SDRoxide
* (GPL-3.0-or-later).
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <glib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rade_api.h"
#include "rade_shim.h"
#include "rade_text.h"
#include "rade_mic_agc.h"
#include "rade_denoiser.h"
#include "rade_resample.h"
#include "rade_engine.h"

#define APP_RATE     48000
#define MODEM_RATE   8000
#define SPEECH_RATE  16000
#define RING_SAMPLES 48000          /* one second */
#define POLL_US      5000           /* control poll, 5 ms */
#define LEVEL_DECAY  0.85f
#define RX_REAL_SCALE 4.0f
#define TX_REAL_SCALE (16384.0f / 32768.0f)
#define SQUELCH_HYSTERESIS_DB   6.0f
#define SQUELCH_SETTLE_FRAMES   4
#define SQUELCH_RAMP_SAMPLES    480.0f
#define MAX_RX_LOG   50
#define EOO_FLOATS   RADE_TEXT_EOO_FLOATS

/*
 * RADE V2 (experimental, branch builds with -DRADE_V2 only): the rade_c
 * library runs its V2 waveform (~860 Hz wide, no pilots, ~3 dB more
 * sensitive). V2 is still under development upstream ("on-air use is not
 * recommended"; waveform and weights may change without notice), so both
 * stations must run builds from the same rade_c commit. V2 has no
 * End-of-Over callsign bits (rade_n_eoo_bits() is 0): the EOO frame still
 * ends the over, but no callsign is sent or decoded. The library's TX
 * bandpass filter is V1 only.
 */
#ifdef RADE_V2
#define RADE_OPEN_MODE RADE_MODE_V2
#define RADE_HAS_EOO_TEXT 0
#else
#define RADE_OPEN_MODE 0
#define RADE_HAS_EOO_TEXT 1
#endif

/* ---------------------------------------------------------------- ring -- */

typedef struct {
  float *buf;
  uint64_t cap;
  atomic_uint_fast64_t head;   /* written by producer */
  atomic_uint_fast64_t tail;   /* written by consumer */
} ring_t;

static int ring_init(ring_t *r, uint64_t cap) {
  r->buf = calloc((size_t)cap, sizeof(float));
  r->cap = cap;
  atomic_store(&r->head, 0);
  atomic_store(&r->tail, 0);
  return r->buf ? 0 : -1;
}

static void ring_free(ring_t *r) { free(r->buf); r->buf = NULL; }

static uint64_t ring_used(ring_t *r) {
  uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
  uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  return h - t;
}

static uint64_t ring_free_slots(ring_t *r) { return r->cap - ring_used(r); }

/* single producer */
static int ring_push(ring_t *r, float v) {
  uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
  uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
  if (h - t >= r->cap) { return -1; }
  r->buf[h % r->cap] = v;
  atomic_store_explicit(&r->head, h + 1, memory_order_release);
  return 0;
}

/* single consumer */
static int ring_pop(ring_t *r, float *v) {
  uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  if (t == h) { return -1; }
  *v = r->buf[t % r->cap];
  atomic_store_explicit(&r->tail, t + 1, memory_order_release);
  return 0;
}

/* ---------------------------------------------------------------- vec ---- */

typedef struct { float *p; size_t n, cap; } fvec;

static void fvec_reserve(fvec *v, size_t extra) {
  if (v->n + extra > v->cap) {
    size_t nc = v->cap ? v->cap : 1024;
    while (nc < v->n + extra) { nc *= 2; }
    v->p = realloc(v->p, nc * sizeof(float));
    v->cap = nc;
  }
}

static void fvec_append(fvec *v, const float *s, size_t n) {
  if (n == 0) { return; }
  fvec_reserve(v, n);
  memcpy(v->p + v->n, s, n * sizeof(float));
  v->n += n;
}

static void fvec_drain(fvec *v, size_t n) {
  if (n >= v->n) { v->n = 0; return; }
  memmove(v->p, v->p + n, (v->n - n) * sizeof(float));
  v->n -= n;
}

/* Resample (or copy when rs == NULL) in -> appended to out. */
static void rs_push(rade_rs_t *rs, int in_rate, int out_rate,
                    const float *in, size_t n, fvec *out) {
  if (n == 0) { return; }
  if (!rs) { fvec_append(out, in, n); return; }
  size_t max_out = n * (size_t)out_rate / (size_t)in_rate + 64;
  fvec_reserve(out, max_out);
  int got = rade_rs_process(rs, in, (int)n, out->p + out->n, (int)max_out);
  if (got > 0) { out->n += (size_t)got; }
}

/* ------------------------------------------------------------ control ---- */

enum { CTL_SET_TX, CTL_SET_CALL, CTL_RESET, CTL_SET_BPF, CTL_STOP };

typedef struct {
  int type;
  int val;
  char call[32];
} ctl_msg;

/* -------------------------------------------------------------- engine --- */

typedef struct {
  struct rade *rade;
  void *venc, *vdec;
  int n_features, n_tx_out, n_tx_eoo_out, nin_max, n_eoo_bits;
  int voc_frame, voc_nfeat;

  ring_t rx_in, rx_out, tx_in, tx_out;
  GAsyncQueue *ctl;
  GThread *thread;

  rade_rs_t *rx_down, *rx_up, *tx_down, *tx_up;

  fvec buf8, buf16, features, tx_features, scratch, audio;
  short *pcm;  int pcm_cap, pcm_n;
  RADE_COMP *iq; int iq_cap, iq_n;
  float eoo_tx[EOO_FLOATS];
  float *eoo_rx;
  float last_sync_snr;

  int transmitting, was_sync, sync_lost_flag;
  float level;
  int gate_open, gate_frames;
  float gate_gain;

  /* shared (atomics) */
  atomic_int sync;
  atomic_int snr_mdb, foff_mhz, level_q16;
  atomic_uint_fast64_t eoo_count, dropped;
  atomic_int tx_finished;
  atomic_int squelch_on, squelch_open_mdb, squelch_gate;
  atomic_int unkeying;
  atomic_int tx_cmd;           /* last tx state sent to the worker (caller side) */

  /* callsign log */
  GMutex log_lock;
  struct { char call[9]; float snr; } log[MAX_RX_LOG];
  int log_n;
} engine_t;

static engine_t *g_eng = NULL;
static atomic_int g_running = 0;

/* mic conditioning (caller thread + UI) */
static GMutex mic_lock;
static rade_denoiser_t *mic_dn = NULL;
static rade_leveler_t mic_lev;
static rade_compressor_t mic_comp;
static rade_eq_t mic_eq;
static int mic_init_done = 0;
static int mic_dn_on, mic_lev_on, mic_comp_on, mic_eq_on;

static void mic_init_once(void) {
  if (mic_init_done) { return; }
  rade_leveler_init(&mic_lev);
  rade_compressor_init(&mic_comp);
  rade_eq_init(&mic_eq);
  mic_init_done = 1;
}

/* ------------------------------------------------------------- helpers --- */

/* Matches rade_c: scale by 32768, clamp to +/-32767, round half up. */
static short f32_to_i16(float s) {
  float v = s * 32768.0f;
  if (v > 32767.0f) { v = 32767.0f; }
  if (v < -32767.0f) { v = -32767.0f; }
  return (short)floor(0.5 + (double)v);
}

static void count_drop(engine_t *e, uint64_t k, const char *what) {
  uint64_t n = atomic_fetch_add(&e->dropped, k) + k;
  if ((n & (n - 1)) == 0) {
    fprintf(stderr, "[rade] %s full, dropped %llu samples total\n", what, (unsigned long long)n);
  }
}

static void push_block(engine_t *e, ring_t *r, const float *a, int n, const char *what) {
  if (n <= 0 || !a) { return; }
  if (ring_free_slots(r) < (uint64_t)n) { count_drop(e, (uint64_t)n, what); return; }
  for (int i = 0; i < n; i++) { ring_push(r, a[i]); }
}

static void reset_rx(engine_t *e) {
  sdrx_voc_dec_reset(e->vdec);
  atomic_store(&e->sync, 0);
  e->level = 0.0f;
}

static void drain_ring(ring_t *r) { float v; while (ring_pop(r, &v) == 0) { } }

/* --------------------------------------------------------------- TX ------ */

/* Real part of iq -> audio rate -> tx_out.  wait_ms > 0 waits for room
 * instead of dropping (used for the End-of-Over frame). */
static void emit_tx_iq(engine_t *e, int wait_ms) {
  if (e->iq_n <= 0) { return; }
  e->scratch.n = 0;
  fvec_reserve(&e->scratch, (size_t)e->iq_n);
  for (int i = 0; i < e->iq_n; i++) { e->scratch.p[i] = e->iq[i].real * TX_REAL_SCALE; }
  e->scratch.n = (size_t)e->iq_n;
  e->audio.n = 0;
  rs_push(e->tx_up, MODEM_RATE, APP_RATE, e->scratch.p, e->scratch.n, &e->audio);
  gint64 deadline = wait_ms > 0 ? g_get_monotonic_time() + (gint64)wait_ms * 1000 : 0;
  for (size_t i = 0; i < e->audio.n; i++) {
    float v = e->audio.p[i];
    if (v > 1.0f) { v = 1.0f; }
    if (v < -1.0f) { v = -1.0f; }
    for (;;) {
      if (ring_push(&e->tx_out, v) == 0) { break; }
      if (deadline && g_get_monotonic_time() < deadline) { g_usleep(1000); continue; }
      count_drop(e, 1, "modem ring");
      break;
    }
  }
}

static void service_tx(engine_t *e) {
  float v;
  e->scratch.n = 0;
  while (ring_pop(&e->tx_in, &v) == 0) { fvec_append(&e->scratch, &v, 1); }
  rs_push(e->tx_down, APP_RATE, SPEECH_RATE, e->scratch.p, e->scratch.n, &e->buf16);

  size_t frame = (size_t)e->voc_frame;
  while (e->buf16.n >= frame) {
    for (size_t i = 0; i < frame; i++) { e->pcm[i] = f32_to_i16(e->buf16.p[i]); }
    fvec_drain(&e->buf16, frame);
    fvec_reserve(&e->tx_features, (size_t)e->voc_nfeat);
    sdrx_voc_enc(e->venc, e->pcm, e->tx_features.p + e->tx_features.n);
    e->tx_features.n += (size_t)e->voc_nfeat;
  }

  size_t n = (size_t)e->n_features;
  while (e->tx_features.n >= n) {
    int got = rade_tx(e->rade, e->iq, e->tx_features.p);
    fvec_drain(&e->tx_features, n);
    if (got < 0) { fprintf(stderr, "[rade] rade_tx failed\n"); continue; }
    e->iq_n = got;
    emit_tx_iq(e, 0);
  }
}

static void set_tx(engine_t *e, int on) {
  if (on == e->transmitting) { return; }
  e->transmitting = on;
  if (on) {
    /* discard anything queued for receive and stale mic audio */
    drain_ring(&e->rx_in);
    e->buf8.n = 0;
    reset_rx(e);
    drain_ring(&e->tx_in);
    e->buf16.n = 0;
    e->tx_features.n = 0;
  } else {
    service_tx(e);
    /* zero-pad the last partial modem frame so the tail of the over goes out */
    if (e->tx_features.n > 0) {
      size_t n = (size_t)e->n_features;
      fvec_reserve(&e->tx_features, n);
      for (size_t i = e->tx_features.n; i < n; i++) { e->tx_features.p[i] = 0.0f; }
      e->tx_features.n = n;
      int got = rade_tx(e->rade, e->iq, e->tx_features.p);
      if (got < 0) { fprintf(stderr, "[rade] rade_tx failed flushing the over\n"); }
      else { e->iq_n = got; emit_tx_iq(e, 3000); }
    }
    /* current callsign goes into the frame about to be sent (V1 only) */
    if (RADE_HAS_EOO_TEXT) { rade_tx_set_eoo_bits(e->rade, e->eoo_tx); }
    int got = rade_tx_eoo(e->rade, e->iq);
    if (got < 0) { fprintf(stderr, "[rade] rade_tx_eoo failed\n"); got = 0; }
    e->iq_n = got;
    emit_tx_iq(e, 3000);
    /* Push silence through the 8k->48k resampler so its tail (the last EOO
     * pilot) comes out instead of staying in the filter. */
    for (int i = 0; i < 1024; i++) { e->iq[i].real = 0.0f; e->iq[i].imag = 0.0f; }
    e->iq_n = 1024;
    emit_tx_iq(e, 3000);
    e->buf16.n = 0;
    e->tx_features.n = 0;
    drain_ring(&e->tx_in);
    atomic_store_explicit(&e->tx_finished, 1, memory_order_release);
  }
}

/* --------------------------------------------------------------- RX ------ */

static int update_gate(engine_t *e) {
  if (!atomic_load(&e->squelch_on)) {
    e->gate_open = 1;
    e->gate_frames = SQUELCH_SETTLE_FRAMES;
    return 1;
  }
  float open_db = (float)atomic_load(&e->squelch_open_mdb) / 1000.0f;
  int sync = rade_sync(e->rade) != 0;
  float snr = rade_snrdB_3k_est(e->rade);
  int was_open = e->gate_open;
  if (e->gate_open) {
    if (!(sync && snr >= open_db - SQUELCH_HYSTERESIS_DB)) { e->gate_open = 0; e->gate_frames = 0; }
  } else if (sync && snr >= open_db) {
    e->gate_frames++;
    if (e->gate_frames >= SQUELCH_SETTLE_FRAMES) { e->gate_open = 1; }
  } else {
    e->gate_frames = 0;
  }
  if (e->gate_open != was_open) {
    fprintf(stderr, "[rade] speech gate %s (sync=%d, snr_db=%.1f, threshold=%.1f)\n",
            e->gate_open ? "opened" : "closed", sync, snr, open_db);
  }
  return e->gate_open;
}

static void publish_rx_state(engine_t *e) {
  int sync = rade_sync(e->rade) != 0;
  if (e->was_sync && !sync) {
    e->sync_lost_flag = 1;
    /* losing sync ends the over: restart the vocoder warm-up */
    sdrx_voc_dec_reset(e->vdec);
  }
  e->was_sync = sync;
  atomic_store(&e->sync, sync);
  if (sync) {
    float snr = rade_snrdB_3k_est(e->rade);
    e->last_sync_snr = snr;   /* latched for the End-of-Over report */
    atomic_store(&e->snr_mdb, (int)(snr * 1000.0f));
    atomic_store(&e->foff_mhz, (int)(rade_freq_offset(e->rade) * 1000.0f));
  }
}

static void synthesize(engine_t *e, int n_feat_floats) {
  int nf = e->voc_nfeat;
  e->pcm_n = 0;
  for (int off = 0; off + nf <= n_feat_floats; off += nf) {
    if (e->pcm_n + e->voc_frame > e->pcm_cap) { break; }
    int r = sdrx_voc_dec(e->vdec, e->features.p + off, e->pcm + e->pcm_n);
    if (r > 0) { e->pcm_n += e->voc_frame; }
  }
  if (e->pcm_n == 0) { return; /* still warming FARGAN up */ }
  e->scratch.n = 0;
  fvec_reserve(&e->scratch, (size_t)e->pcm_n);
  float peak = 0.0f;
  for (int i = 0; i < e->pcm_n; i++) {
    float s = (float)e->pcm[i] / 32768.0f;
    e->scratch.p[i] = s;
    if (fabsf(s) > peak) { peak = fabsf(s); }
  }
  e->scratch.n = (size_t)e->pcm_n;
  e->level = e->level * LEVEL_DECAY;
  if (peak > e->level) { e->level = peak; }
  float lv = e->level < 0.0f ? 0.0f : (e->level > 1.0f ? 1.0f : e->level);
  atomic_store(&e->level_q16, (int)(lv * 65536.0f));

  e->audio.n = 0;
  rs_push(e->rx_up, SPEECH_RATE, APP_RATE, e->scratch.p, e->scratch.n, &e->audio);
  /* the vocoder always runs (keeps FARGAN continuous); only output is gated */
  float target = update_gate(e) ? 1.0f : 0.0f;
  atomic_store(&e->squelch_gate, e->gate_open);
  float step = 1.0f / SQUELCH_RAMP_SAMPLES;
  for (size_t i = 0; i < e->audio.n; i++) {
    if (e->gate_gain < target) { e->gate_gain += step; if (e->gate_gain > target) { e->gate_gain = target; } }
    else if (e->gate_gain > target) { e->gate_gain -= step; if (e->gate_gain < target) { e->gate_gain = target; } }
    float s = e->audio.p[i] * e->gate_gain;
    if (s > 1.0f) { s = 1.0f; }
    if (s < -1.0f) { s = -1.0f; }
    if (ring_push(&e->rx_out, s) != 0) { count_drop(e, 1, "speech ring"); }
  }
}

static void handle_eoo(engine_t *e) {
  /* the over has ended: nothing after this is speech */
  e->gate_open = 0;
  e->gate_frames = 0;
  atomic_store(&e->squelch_gate, 0);
  atomic_fetch_add(&e->eoo_count, 1);
  char call[9];
  if (!RADE_HAS_EOO_TEXT) {
    fprintf(stderr, "[rade] End-of-Over (V2: no callsign) (snr_db=%.1f)\n", e->last_sync_snr);
  } else if (rade_text_decode(e->eoo_rx, call)) {
    fprintf(stderr, "[rade] End-of-Over callsign %s (snr_db=%.1f)\n", call, e->last_sync_snr);
    g_mutex_lock(&e->log_lock);
    if (e->log_n >= MAX_RX_LOG) {
      memmove(&e->log[0], &e->log[1], sizeof(e->log[0]) * (MAX_RX_LOG - 1));
      e->log_n = MAX_RX_LOG - 1;
    }
    memcpy(e->log[e->log_n].call, call, 9);
    e->log[e->log_n].snr = e->last_sync_snr;
    e->log_n++;
    g_mutex_unlock(&e->log_lock);
  } else {
    fprintf(stderr, "[rade] End-of-Over frame detected but text decode failed (snr_db=%.1f)\n",
            e->last_sync_snr);
  }
}

static void service_rx(engine_t *e) {
  float v;
  e->scratch.n = 0;
  while (ring_pop(&e->rx_in, &v) == 0) { fvec_append(&e->scratch, &v, 1); }
  if (e->scratch.n == 0 && e->buf8.n < (size_t)rade_nin(e->rade)) { return; }
  rs_push(e->rx_down, APP_RATE, MODEM_RATE, e->scratch.p, e->scratch.n, &e->buf8);

  /* nin moves between calls as timing tracking pulls the clock */
  while (e->buf8.n >= (size_t)rade_nin(e->rade)) {
    int nin = rade_nin(e->rade);
    if (nin > e->iq_cap) {
      fprintf(stderr, "[rade] nin %d too large\n", nin);
      fvec_drain(&e->buf8, (size_t)nin);
      continue;
    }
    for (int i = 0; i < nin; i++) { e->iq[i].real = e->buf8.p[i] * RX_REAL_SCALE; e->iq[i].imag = 0.0f; }
    fvec_drain(&e->buf8, (size_t)nin);

    int has_eoo = 0;
    memset(e->features.p, 0, (size_t)e->n_features * sizeof(float));
    int n = rade_rx(e->rade, e->features.p, &has_eoo, RADE_HAS_EOO_TEXT ? e->eoo_rx : NULL, e->iq);
    if (n < 0) { fprintf(stderr, "[rade] rade_rx failed (%d)\n", n); continue; }
    /* before EOO handling, so a frame still in sync refreshes the SNR */
    publish_rx_state(e);
    if (e->sync_lost_flag) {
      e->sync_lost_flag = 0;
      fprintf(stderr, "[rade] over ended: sync lost (last snr_db=%.1f), End-of-Over flagged in the same step: %d\n",
              e->last_sync_snr, has_eoo != 0);
    }
    if (has_eoo) { handle_eoo(e); }
    if (n == 0) { continue; }
    synthesize(e, n);
  }
}

/* -------------------------------------------------------------- thread --- */

static void apply_ctl(engine_t *e, ctl_msg *m) {
  switch (m->type) {
  case CTL_SET_TX:   set_tx(e, m->val); break;
  case CTL_SET_CALL: rade_text_encode(m->call, e->eoo_tx); break;
  case CTL_RESET:    reset_rx(e); break;
  case CTL_SET_BPF:  if (RADE_HAS_EOO_TEXT) { rade_set_tx_bpf(e->rade, m->val); } break;   /* V1 only */
  default: break;
  }
}

static gpointer worker_main(gpointer arg) {
  engine_t *e = (engine_t *)arg;
  for (;;) {
    ctl_msg *m = g_async_queue_timeout_pop(e->ctl, POLL_US);
    if (m) {
      int stop = (m->type == CTL_STOP);
      if (!stop) { apply_ctl(e, m); }
      g_free(m);
      if (stop) { break; }
    }
    if (e->transmitting) { service_tx(e); } else { service_rx(e); }
  }
  fprintf(stderr, "[rade] worker stopped\n");
  return NULL;
}

static void send_ctl(engine_t *e, int type, int val, const char *call) {
  ctl_msg *m = g_new0(ctl_msg, 1);
  m->type = type;
  m->val = val;
  if (call) { g_strlcpy(m->call, call, sizeof(m->call)); }
  g_async_queue_push(e->ctl, m);
}

static void engine_free(engine_t *e) {
  if (!e) { return; }
  if (e->rade) { rade_close(e->rade); }
  if (e->venc) { sdrx_voc_enc_destroy(e->venc); }
  if (e->vdec) { sdrx_voc_dec_destroy(e->vdec); }
  rade_rs_free(e->rx_down); rade_rs_free(e->rx_up);
  rade_rs_free(e->tx_down); rade_rs_free(e->tx_up);
  ring_free(&e->rx_in); ring_free(&e->rx_out); ring_free(&e->tx_in); ring_free(&e->tx_out);
  if (e->ctl) {
    ctl_msg *m;
    while ((m = g_async_queue_try_pop(e->ctl)) != NULL) { g_free(m); }
    g_async_queue_unref(e->ctl);
  }
  free(e->buf8.p); free(e->buf16.p); free(e->features.p); free(e->tx_features.p);
  free(e->scratch.p); free(e->audio.p); free(e->pcm); free(e->iq); free(e->eoo_rx);
  g_mutex_clear(&e->log_lock);
  free(e);
}

int rade_engine_start(void) {
  int expected = 0;
  if (!atomic_compare_exchange_strong(&g_running, &expected, 1)) { return -1; }
  engine_t *e = calloc(1, sizeof(*e));
  if (!e) { atomic_store(&g_running, 0); return -1; }
  g_mutex_init(&e->log_lock);

  static int initialised = 0;
  if (!initialised) { rade_initialize(); initialised = 1; }
  char model[1] = { 0 };
  e->rade = rade_open(model, RADE_VERBOSE_0 | RADE_OPEN_MODE);   /* never the verbose flags */
  if (!e->rade) { fprintf(stderr, "[rade] rade_open failed\n"); goto fail; }
  e->n_features = rade_n_features_in_out(e->rade);
  e->n_tx_out = rade_n_tx_out(e->rade);
  e->n_tx_eoo_out = rade_n_tx_eoo_out(e->rade);
  e->nin_max = rade_nin_max(e->rade);
  e->n_eoo_bits = rade_n_eoo_bits(e->rade);
  e->voc_frame = sdrx_voc_frame_size();
  e->voc_nfeat = sdrx_voc_n_features();
  fprintf(stderr, "[rade] RADE %s: %d features, %d tx samples, %d eoo samples, nin max %d, %d eoo bits\n",
          RADE_OPEN_MODE ? "V2 (experimental)" : "V1", e->n_features, e->n_tx_out, e->n_tx_eoo_out,
          e->nin_max, e->n_eoo_bits);
  if (RADE_HAS_EOO_TEXT && e->n_eoo_bits != EOO_FLOATS) {
    fprintf(stderr, "[rade] unexpected eoo bits %d\n", e->n_eoo_bits);
    goto fail;
  }

  e->venc = sdrx_voc_enc_create();
  e->vdec = sdrx_voc_dec_create();
  if (!e->venc || !e->vdec) { goto fail; }

  if (ring_init(&e->rx_in, RING_SAMPLES) || ring_init(&e->rx_out, RING_SAMPLES) ||
      ring_init(&e->tx_in, RING_SAMPLES) || ring_init(&e->tx_out, RING_SAMPLES)) { goto fail; }

  e->rx_down = rade_rs_new(APP_RATE, MODEM_RATE);
  e->rx_up = rade_rs_new(SPEECH_RATE, APP_RATE);
  e->tx_down = rade_rs_new(APP_RATE, SPEECH_RATE);
  e->tx_up = rade_rs_new(MODEM_RATE, APP_RATE);
  if (!e->rx_down || !e->rx_up || !e->tx_down || !e->tx_up) { goto fail; }

  e->iq_cap = e->nin_max;
  if (e->n_tx_out > e->iq_cap) { e->iq_cap = e->n_tx_out; }
  if (e->n_tx_eoo_out > e->iq_cap) { e->iq_cap = e->n_tx_eoo_out; }
  if (e->iq_cap < 1024) { e->iq_cap = 1024; }
  e->iq = calloc((size_t)e->iq_cap, sizeof(RADE_COMP));
  e->pcm_cap = (e->n_features / e->voc_nfeat + 2) * e->voc_frame;
  if (e->pcm_cap < e->voc_frame) { e->pcm_cap = e->voc_frame; }
  e->pcm = calloc((size_t)e->pcm_cap, sizeof(short));
  e->eoo_rx = calloc((size_t)(e->n_eoo_bits > 0 ? e->n_eoo_bits : 1), sizeof(float));
  fvec_reserve(&e->features, (size_t)e->n_features);
  e->features.n = (size_t)e->n_features;
  if (!e->iq || !e->pcm || !e->eoo_rx || !e->features.p) { goto fail; }

  /* never leave the EOO payload at the all-zero default */
  rade_text_encode("", e->eoo_tx);

  atomic_store(&e->squelch_on, 0);
  atomic_store(&e->squelch_open_mdb, -4000);
  e->ctl = g_async_queue_new();

  g_eng = e;
  e->thread = g_thread_new("rade", worker_main, e);
  fprintf(stderr, "[rade] worker started (48 kHz audio)\n");
  return 0;

fail:
  engine_free(e);
  atomic_store(&g_running, 0);
  return -1;
}

void rade_engine_stop(void) {
  engine_t *e = g_eng;
  if (!e) { return; }
  send_ctl(e, CTL_STOP, 0, NULL);
  g_thread_join(e->thread);
  g_eng = NULL;
  engine_free(e);
  atomic_store(&g_running, 0);
}

int rade_engine_is_running(void) { return g_eng != NULL; }

/* ----------------------------------------------------------- public API -- */

void rade_engine_push_rx(const float *mono48, int n) {
  engine_t *e = g_eng;
  if (e) { push_block(e, &e->rx_in, mono48, n, "rx ring"); }
}

int rade_engine_pop_rx(float *out, int n) {
  engine_t *e = g_eng;
  int k = 0;
  if (!e || !out) { return 0; }
  while (k < n && ring_pop(&e->rx_out, &out[k]) == 0) { k++; }
  return k;
}

int rade_engine_rx_free(void) {
  engine_t *e = g_eng;
  return e ? (int)ring_free_slots(&e->rx_in) : 0;
}

void rade_engine_reset_rx(void) { if (g_eng) { send_ctl(g_eng, CTL_RESET, 0, NULL); } }

void rade_engine_set_squelch(int on, float open_snr_db) {
  engine_t *e = g_eng;
  if (!e) { return; }
  atomic_store(&e->squelch_on, on != 0);
  atomic_store(&e->squelch_open_mdb, (int)(open_snr_db * 1000.0f));
}

void rade_engine_push_mic(const float *mic48, int n) {
  engine_t *e = g_eng;
  if (e) { push_block(e, &e->tx_in, mic48, n, "mic ring"); }
}

int rade_engine_pop_tx(float *out48, int n) {
  engine_t *e = g_eng;
  int k = 0;
  if (!out48) { return 0; }
  for (int i = 0; i < n; i++) {
    float v;
    if (e && ring_pop(&e->tx_out, &v) == 0) { out48[i] = v; k++; } else { out48[i] = 0.0f; }
  }
  return k;
}

void rade_engine_set_tx(int on) {
  engine_t *e = g_eng;
  if (!e) { return; }
  on = on != 0;
  if (atomic_exchange(&e->tx_cmd, on) == on) { return; }
  if (on) { atomic_store_explicit(&e->tx_finished, 0, memory_order_release); }
  send_ctl(e, CTL_SET_TX, on, NULL);
}

int rade_engine_tx_fill(int mox, const float *mic48, int n, float *out48) {
  engine_t *e = g_eng;
  if (!e) {
    if (out48) { memset(out48, 0, (size_t)n * sizeof(float)); }
    return 0;
  }
  int on = mox && !atomic_load(&e->unkeying);
  int was_on = atomic_load(&e->tx_cmd);
  rade_engine_set_tx(on);
  /* Deviation from the Rust fill_tx: mic audio is only fed while keyed (and
   * for the call that unkeys), so idle audio does not inflate `dropped`. */
  if (on || was_on) { rade_engine_push_mic(mic48, n); }
  return rade_engine_pop_tx(out48, n);
}

void rade_engine_set_unkeying(int on) {
  if (g_eng) { atomic_store(&g_eng->unkeying, on != 0); }
}

int rade_engine_tx_drained(void) {
  engine_t *e = g_eng;
  if (!e) { return 1; }
  return atomic_load_explicit(&e->tx_finished, memory_order_acquire) && ring_used(&e->tx_out) == 0;
}

int rade_engine_tx_pending(void) {
  engine_t *e = g_eng;
  return e ? (int)ring_used(&e->tx_in) : 0;
}

void rade_engine_set_callsign(const char *call) {
  if (g_eng) { send_ctl(g_eng, CTL_SET_CALL, 0, call ? call : ""); }
}

void rade_engine_set_tx_bpf(int enable) {
  if (g_eng) { send_ctl(g_eng, CTL_SET_BPF, enable != 0, NULL); }
}

void rade_engine_get_stats(rade_engine_stats_t *st) {
  engine_t *e = g_eng;
  memset(st, 0, sizeof(*st));
  if (!e) { return; }
  st->sync = atomic_load(&e->sync);
  st->snr_db = (float)atomic_load(&e->snr_mdb) / 1000.0f;
  st->foff_hz = (float)atomic_load(&e->foff_mhz) / 1000.0f;
  st->level = (float)atomic_load(&e->level_q16) / 65536.0f;
  st->eoo_count = atomic_load(&e->eoo_count);
  st->dropped = atomic_load(&e->dropped);
  st->squelch_open = atomic_load(&e->squelch_gate);
  st->tx_finished = atomic_load(&e->tx_finished);
}

int rade_engine_next_callsign(char call[9], float *snr_db) {
  engine_t *e = g_eng;
  int got = 0;
  if (!e) { return 0; }
  g_mutex_lock(&e->log_lock);
  if (e->log_n > 0) {
    memcpy(call, e->log[0].call, 9);
    if (snr_db) { *snr_db = e->log[0].snr; }
    memmove(&e->log[0], &e->log[1], sizeof(e->log[0]) * (size_t)(e->log_n - 1));
    e->log_n--;
    got = 1;
  }
  g_mutex_unlock(&e->log_lock);
  return got;
}

/* ------------------------------------------------------ mic conditioning -- */

void rade_engine_set_mic_denoise(int on) {
  g_mutex_lock(&mic_lock);
  mic_dn_on = on != 0;
  g_mutex_unlock(&mic_lock);
}

void rade_engine_set_mic_leveler(int on) {
  g_mutex_lock(&mic_lock);
  mic_init_once();
  if (on && !mic_lev_on) { rade_leveler_reset(&mic_lev); }
  mic_lev_on = on != 0;
  g_mutex_unlock(&mic_lock);
}

void rade_engine_set_mic_compressor(int on) {
  g_mutex_lock(&mic_lock);
  mic_init_once();
  if (on && !mic_comp_on) { rade_compressor_reset(&mic_comp); }
  mic_comp_on = on != 0;
  g_mutex_unlock(&mic_lock);
}

void rade_engine_set_mic_eq(int on) {
  g_mutex_lock(&mic_lock);
  mic_init_once();
  if (on && !mic_eq_on) { rade_eq_reset(&mic_eq); }
  mic_eq_on = on != 0;
  g_mutex_unlock(&mic_lock);
}

void rade_engine_set_mic_eq_params(float bass_hz, float bass_db, float mid_hz, float mid_db,
                                   float mid_q, float treble_hz, float treble_db, float vol_db) {
  g_mutex_lock(&mic_lock);
  mic_init_once();
  rade_eq_set_bass(&mic_eq, bass_hz, bass_db);
  rade_eq_set_mid(&mic_eq, mid_hz, mid_db, mid_q);
  rade_eq_set_treble(&mic_eq, treble_hz, treble_db);
  rade_eq_set_vol(&mic_eq, vol_db);
  g_mutex_unlock(&mic_lock);
}

void rade_engine_condition_mic(float *buf, int n) {
  if (!buf || n <= 0) { return; }
  g_mutex_lock(&mic_lock);
  mic_init_once();
  if (mic_dn_on) {
    if (!mic_dn) { mic_dn = rade_denoiser_new(); }
    if (mic_dn) { rade_denoiser_process(mic_dn, buf, n); }
  }
  if (mic_lev_on) { rade_leveler_process(&mic_lev, buf, n, APP_RATE); }
  if (mic_comp_on) { rade_compressor_process(&mic_comp, buf, n, APP_RATE); }
  if (mic_eq_on) { rade_eq_process(&mic_eq, buf, n, APP_RATE); }
  g_mutex_unlock(&mic_lock);
}
