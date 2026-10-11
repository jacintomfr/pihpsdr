/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
 * RADE mic conditioning: leveler, soft-knee compressor/limiter, 3-band EQ.
 * All process functions work in place on float blocks (+/-1.0).
 */
#ifndef RADE_MIC_AGC_H
#define RADE_MIC_AGC_H

#define RADE_EQ_BASS_FREQ_MAX_HZ   600.0f
#define RADE_EQ_MID_FREQ_MAX_HZ    3900.0f
#define RADE_EQ_TREBLE_FREQ_MAX_HZ 3900.0f
#define RADE_EQ_GAIN_MIN_DB        -20.0f
#define RADE_EQ_GAIN_MAX_DB        20.0f
#define RADE_EQ_Q_MIN              0.1f
#define RADE_EQ_Q_MAX              10.0f

typedef struct {
  float current_gain_db;
} rade_leveler_t;

typedef struct {
  float envelope_db;
} rade_compressor_t;

typedef struct {
  float x1, x2, y1, y2;
  float b0, b1, b2, a1, a2;
} rade_biquad_t;

typedef struct {
  float bass_freq_hz, bass_gain_db;
  float mid_freq_hz, mid_gain_db, mid_q;
  float treble_freq_hz, treble_gain_db;
  float vol_gain_db;
  rade_biquad_t bass, mid, treble;
} rade_eq_t;

void rade_leveler_init(rade_leveler_t *l);
void rade_leveler_reset(rade_leveler_t *l);
void rade_leveler_process(rade_leveler_t *l, float *block, int n, double sample_rate);

void rade_compressor_init(rade_compressor_t *c);
void rade_compressor_reset(rade_compressor_t *c);
void rade_compressor_process(rade_compressor_t *c, float *block, int n, double sample_rate);

void rade_eq_init(rade_eq_t *e);
void rade_eq_reset(rade_eq_t *e);  /* clears filter history only */
/* Setters clamp to the freedv-gui slider ranges. */
void rade_eq_set_bass(rade_eq_t *e, float freq_hz, float gain_db);
void rade_eq_set_mid(rade_eq_t *e, float freq_hz, float gain_db, float q);
void rade_eq_set_treble(rade_eq_t *e, float freq_hz, float gain_db);
void rade_eq_set_vol(rade_eq_t *e, float gain_db);
void rade_eq_process(rade_eq_t *e, float *block, int n, double sample_rate);

#endif
