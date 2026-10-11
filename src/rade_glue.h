/* Copyright (C)
* RADE V1 integration glue for piHPSDR-IF (RX tap, TX substitution, unkeying,
* persistence), ported from the deskHPSDR Windows RADE port.  Everything here is compiled only with -DRADE.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef _RADE_GLUE_H
#define _RADE_GLUE_H

#ifdef RADE

#include "rade_engine.h"

/* ---- hooks called from shared code (one line each) ---- */
/* receiver.c: replace the 48 kHz stereo WDSP output of RX1 by decoded speech */
void rade_glue_rx_block(int rx_id, double *stereo48, int frames);
/* transmitter.c: 1 if mic samples are being replaced by RADE modem audio */
int  rade_glue_tx_active(void);
/* transmitter.c: returns the modem audio sample that replaces the mic sample */
double rade_glue_tx_sample(double mic_sample);
/* radio.c radio_set_mox(0): returns 1 if the MOX-off was taken over (the
 * End-of-Over burst is being sent, real MOX drops later) */
int  rade_glue_mox_off_intercept(void);
/* radio.c radio_set_mox(1): cancels a pending unkey */
void rade_glue_mox_on(void);
/* radio.c property load/save (server part) and radio_stop_program() */
void rade_glue_load_state(void);
void rade_glue_save_state(void);
void rade_glue_shutdown(void);

/* ---- API for the (future) menu ---- */
void rade_glue_set_rx_enabled(int on);   /* starts/stops the engine as needed */
int  rade_glue_get_rx_enabled(void);
/* not persisted; GTK thread. While armed: WDSP compressor/leveler/EQ/CFC forced off, TX filter 700-2300 Hz, user values restored on disarm */
void rade_glue_set_tx_armed(int on);
int  rade_glue_get_tx_armed(void);
int  rade_glue_engine_running(void);
void rade_glue_set_squelch(int on);      /* RX speech gate, not persisted */
int  rade_glue_get_squelch(void);
/* own callsign sent in the End-of-Over frame (upper case, no spaces; saved) */
void rade_glue_set_callsign(const char *call);
const char *rade_glue_get_callsign_own(void);

void rade_glue_set_denoiser(int on);
void rade_glue_set_leveler(int on);
void rade_glue_set_compressor(int on);
void rade_glue_set_eq(int on);
void rade_glue_set_mute_analog(int on);
void rade_glue_set_filter_wide(int on);  /* stored only; menu applies the RX/TX filter */
int  rade_glue_get_denoiser(void);
int  rade_glue_get_leveler(void);
int  rade_glue_get_compressor(void);
int  rade_glue_get_eq(void);
void  rade_glue_set_eq_gain(int band, float db);   /* band 0 bass, 1 mid, 2 treble, 3 volume; dB, +-20 */
float rade_glue_get_eq_gain(int band);
int  rade_glue_get_mute_analog(void);
int  rade_glue_get_filter_wide(void);

void rade_glue_get_stats(rade_engine_stats_t *st);
/* Number of remembered received callsigns (max RADE_GLUE_HISTORY), newest first. */
#define RADE_GLUE_HISTORY 8
int  rade_glue_callsign_count(void);
int  rade_glue_get_callsign(int idx, char call[9], float *snr_db);

#endif /* RADE */
#endif
