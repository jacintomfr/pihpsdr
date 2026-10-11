/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <math.h>
#include "rade_mic_agc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AGC_TARGET_DBFS  -23.0f
#define AGC_MAX_GAIN_DB   12.0f
#define AGC_MIN_GAIN_DB  -20.0f
#define AGC_ATTACK_SEC     0.5f
#define AGC_RELEASE_SEC    6.0f
#define AGC_SILENCE_DBFS -33.0f

#define COMP_THRESHOLD_DBFS -12.0f
#define COMP_RATIO            3.0f
#define COMP_KNEE_DB          6.0f
#define COMP_CEILING_DBFS    -1.0f
#define COMP_ATTACK_MS        5.0f
#define COMP_RELEASE_MS     250.0f

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- leveler ---- */

void rade_leveler_init(rade_leveler_t *l) { l->current_gain_db = 0.0f; }
void rade_leveler_reset(rade_leveler_t *l) { l->current_gain_db = 0.0f; }

void rade_leveler_process(rade_leveler_t *l, float *block, int n, double sample_rate) {
  if (n <= 0 || sample_rate <= 0.0) { return; }
  float sum = 0.0f;
  for (int i = 0; i < n; i++) { sum += block[i] * block[i]; }
  float rms = sqrtf(sum / (float)n);
  if (rms < 1e-9f) { rms = 1e-9f; }
  float level_dbfs = 20.0f * log10f(rms);
  if (level_dbfs > AGC_SILENCE_DBFS) {
    float target = clampf(AGC_TARGET_DBFS - level_dbfs, AGC_MIN_GAIN_DB, AGC_MAX_GAIN_DB);
    float interval = target < l->current_gain_db ? AGC_ATTACK_SEC : AGC_RELEASE_SEC;
    float step = (target - l->current_gain_db) / interval * ((float)n / (float)sample_rate);
    l->current_gain_db = clampf(l->current_gain_db + step, AGC_MIN_GAIN_DB, AGC_MAX_GAIN_DB);
  }
  float scale = powf(10.0f, l->current_gain_db / 20.0f);
  for (int i = 0; i < n; i++) { block[i] = clampf(block[i] * scale, -1.0f, 1.0f); }
}

/* ---- compressor / limiter ---- */

void rade_compressor_init(rade_compressor_t *c) { c->envelope_db = -90.0f; }
void rade_compressor_reset(rade_compressor_t *c) { c->envelope_db = -90.0f; }

void rade_compressor_process(rade_compressor_t *c, float *block, int n, double sample_rate) {
  if (n <= 0 || sample_rate <= 0.0) { return; }
  float sr = (float)sample_rate;
  float attack = expf(-1.0f / (COMP_ATTACK_MS / 1000.0f * sr));
  float release = expf(-1.0f / (COMP_RELEASE_MS / 1000.0f * sr));
  const float half_knee = COMP_KNEE_DB / 2.0f;
  const float slope = 1.0f - 1.0f / COMP_RATIO;
  for (int i = 0; i < n; i++) {
    float a = fabsf(block[i]);
    if (a < 1e-9f) { a = 1e-9f; }
    float in_db = 20.0f * log10f(a);
    float coef = in_db > c->envelope_db ? attack : release;
    c->envelope_db = coef * c->envelope_db + (1.0f - coef) * in_db;
    float over = c->envelope_db - COMP_THRESHOLD_DBFS;
    float red;
    if (over <= -half_knee) {
      red = 0.0f;
    } else if (over >= half_knee) {
      red = over * slope;
    } else {
      float x = over + half_knee;
      red = (x * x) / (2.0f * COMP_KNEE_DB) * slope;
    }
    float post = c->envelope_db - red;
    float over_ceiling = post - COMP_CEILING_DBFS;
    if (over_ceiling < 0.0f) { over_ceiling = 0.0f; }
    float gain = powf(10.0f, -(red + over_ceiling) / 20.0f);
    block[i] = clampf(block[i] * gain, -1.0f, 1.0f);
  }
}

/* ---- equalizer (RBJ cookbook biquads, Direct Form I) ---- */

static void bq_identity(rade_biquad_t *b) {
  b->b0 = 1.0f;
  b->b1 = b->b2 = b->a1 = b->a2 = 0.0f;
  b->x1 = b->x2 = b->y1 = b->y2 = 0.0f;
}

static void bq_reset(rade_biquad_t *b) { b->x1 = b->x2 = b->y1 = b->y2 = 0.0f; }

/* kind: 0 low shelf, 1 high shelf, 2 peaking.  Keeps filter history. */
static void bq_design(rade_biquad_t *b, int kind, float freq, float gain_db, float q, double sr) {
  float a = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * (float)M_PI * freq / (float)sr;
  float s = sinf(w0), c = cosf(w0);
  if (kind == 2) {
    float alpha = s / (2.0f * (q > 0.01f ? q : 0.01f));
    float a0 = 1.0f + alpha / a;
    b->b0 = (1.0f + alpha * a) / a0;
    b->b1 = -2.0f * c / a0;
    b->b2 = (1.0f - alpha * a) / a0;
    b->a1 = -2.0f * c / a0;
    b->a2 = (1.0f - alpha / a) / a0;
    return;
  }
  float alpha = s / 2.0f * sqrtf(2.0f);
  float t = 2.0f * sqrtf(a) * alpha;
  if (kind == 0) {
    float a0 = (a + 1.0f) + (a - 1.0f) * c + t;
    b->b0 = a * ((a + 1.0f) - (a - 1.0f) * c + t) / a0;
    b->b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * c) / a0;
    b->b2 = a * ((a + 1.0f) - (a - 1.0f) * c - t) / a0;
    b->a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * c) / a0;
    b->a2 = ((a + 1.0f) + (a - 1.0f) * c - t) / a0;
  } else {
    float a0 = (a + 1.0f) - (a - 1.0f) * c + t;
    b->b0 = a * ((a + 1.0f) + (a - 1.0f) * c + t) / a0;
    b->b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * c) / a0;
    b->b2 = a * ((a + 1.0f) + (a - 1.0f) * c - t) / a0;
    b->a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * c) / a0;
    b->a2 = ((a + 1.0f) - (a - 1.0f) * c - t) / a0;
  }
}

static inline float bq_sample(rade_biquad_t *b, float x) {
  float y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2;
  b->x2 = b->x1;
  b->x1 = x;
  b->y2 = b->y1;
  b->y1 = y;
  return y;
}

void rade_eq_init(rade_eq_t *e) {
  e->bass_freq_hz = 100.0f;
  e->bass_gain_db = 0.0f;
  e->mid_freq_hz = 1500.0f;
  e->mid_gain_db = 0.0f;
  e->mid_q = 1.0f;
  e->treble_freq_hz = 3000.0f;
  e->treble_gain_db = 0.0f;
  e->vol_gain_db = 0.0f;
  bq_identity(&e->bass);
  bq_identity(&e->mid);
  bq_identity(&e->treble);
}

void rade_eq_reset(rade_eq_t *e) {
  bq_reset(&e->bass);
  bq_reset(&e->mid);
  bq_reset(&e->treble);
}

void rade_eq_set_bass(rade_eq_t *e, float freq_hz, float gain_db) {
  e->bass_freq_hz = clampf(freq_hz, 1.0f, RADE_EQ_BASS_FREQ_MAX_HZ);
  e->bass_gain_db = clampf(gain_db, RADE_EQ_GAIN_MIN_DB, RADE_EQ_GAIN_MAX_DB);
}

void rade_eq_set_mid(rade_eq_t *e, float freq_hz, float gain_db, float q) {
  e->mid_freq_hz = clampf(freq_hz, 1.0f, RADE_EQ_MID_FREQ_MAX_HZ);
  e->mid_gain_db = clampf(gain_db, RADE_EQ_GAIN_MIN_DB, RADE_EQ_GAIN_MAX_DB);
  e->mid_q = clampf(q, RADE_EQ_Q_MIN, RADE_EQ_Q_MAX);
}

void rade_eq_set_treble(rade_eq_t *e, float freq_hz, float gain_db) {
  e->treble_freq_hz = clampf(freq_hz, 1.0f, RADE_EQ_TREBLE_FREQ_MAX_HZ);
  e->treble_gain_db = clampf(gain_db, RADE_EQ_GAIN_MIN_DB, RADE_EQ_GAIN_MAX_DB);
}

void rade_eq_set_vol(rade_eq_t *e, float gain_db) {
  e->vol_gain_db = clampf(gain_db, RADE_EQ_GAIN_MIN_DB, RADE_EQ_GAIN_MAX_DB);
}

void rade_eq_process(rade_eq_t *e, float *block, int n, double sample_rate) {
  if (n <= 0 || sample_rate <= 0.0) { return; }
  bq_design(&e->bass, 0, e->bass_freq_hz, e->bass_gain_db, 0.0f, sample_rate);
  bq_design(&e->mid, 2, e->mid_freq_hz, e->mid_gain_db, e->mid_q, sample_rate);
  bq_design(&e->treble, 1, e->treble_freq_hz, e->treble_gain_db, 0.0f, sample_rate);
  float vol = powf(10.0f, e->vol_gain_db / 20.0f);
  for (int i = 0; i < n; i++) {
    float y = bq_sample(&e->bass, block[i]);
    y = bq_sample(&e->mid, y);
    y = bq_sample(&e->treble, y);
    block[i] = clampf(y * vol, -1.0f, 1.0f);
  }
}
