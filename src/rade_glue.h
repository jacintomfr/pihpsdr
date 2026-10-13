/* Copyright (C)
* RADE V1 integration glue for piHPSDR-IF (RX tap, TX substitution, unkeying,
* persistence), ported from the deskHPSDR Windows RADE port.  Everything here is compiled only with -DRADE.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef _RADE_GLUE_H
#define _RADE_GLUE_H

#ifdef RADE

/*
 * TX filter and RX "Fit Filter" passbands. RADE V1 fills about 700 - 2300 Hz;
 * RADE V2 (measured from the library, 2026-10-13: peak 1450 Hz, 99 % of the
 * power in 1080 - 1900 Hz, 820 Hz) is narrower, so its filters are too, with
 * some room for a mistuned station on receive.
 */
#ifdef RADE_V2
#define RADE_TX_LOW        1000
#define RADE_TX_HIGH       2000
#define RADE_RX_NARROW_LOW  950
#define RADE_RX_NARROW_HIGH 2050
#define RADE_RX_WIDE_LOW    800
#define RADE_RX_WIDE_HIGH  2200
#define RADE_FIT_LABEL_WIDE   "RX filter: 800-2200 Hz (wide)"
#define RADE_FIT_LABEL_NARROW "RX filter: 950-2050 Hz (narrow)"
#else
#define RADE_TX_LOW  700
#define RADE_TX_HIGH 2300
#define RADE_RX_NARROW_LOW  700
#define RADE_RX_NARROW_HIGH 2300
#define RADE_RX_WIDE_LOW    300
#define RADE_RX_WIDE_HIGH  2700
#define RADE_FIT_LABEL_WIDE   "RX filter: 300-2700 Hz (wide)"
#define RADE_FIT_LABEL_NARROW "RX filter: 700-2300 Hz (narrow)"
#endif

#include "rade_engine.h"

/* piHPSDR-IF: TX Drive (= TS PWR, watts) limit while RADE TX is armed. RADE
 * is a continuous, full-duty signal: 50 W keeps the TS-570 PA cool and more
 * linear (like the TS limits AM / FM power itself). */
#define RADE_DRIVE_MAX 50.0

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

/* Quick Tune (index into its band list) and Fit Filter (wide <-> narrow);
 * on a client they are carried out by the server */
int  rade_glue_quick_tune_count(void);
const char *rade_glue_quick_tune_label(int i, long long *hz);
void rade_glue_quick_tune(int i);
void rade_glue_fit_filter(void);

/* client / server (client_server.h): server packs its state for the client,
 * the client applies it; the server carries out the client's menu actions */
struct _rade_data;
void rade_glue_fill_remote(struct _rade_data *d);
void rade_glue_client_update(const struct _rade_data *d);
void rade_glue_remote_command(int what, int b2, int value, const char *text);

#endif /* RADE */
#endif
