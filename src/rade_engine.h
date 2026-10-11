/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* RADE V1 engine (C port of the hpsdr-rs Rust worker, itself adapted from
* SDRoxide, GPL-3.0-or-later).
*
* SPDX-License-Identifier: GPL-3.0-or-later
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

#ifndef _RADE_ENGINE_H
#define _RADE_ENGINE_H

#include <stdint.h>

/*
 * All application audio is mono float at 48 kHz.  One worker thread does the
 * modem, vocoder and every sample-rate conversion; audio crosses to/from it
 * through four lock-free SPSC rings of 48000 samples (overflow is dropped and
 * counted).  Only one engine can exist per process.
 *
 * Thread contract: push_rx / tx_fill (or push_mic / pop_tx) from one audio
 * thread, pop_rx from one audio thread (may be the same), everything else
 * from any thread.  condition_mic must be called from the thread that
 * produces the mic audio.
 */

typedef struct {
  int      sync;          /* receiver locked */
  float    snr_db;        /* 3 kHz SNR estimate, valid/updated while sync */
  float    foff_hz;       /* frequency offset, valid/updated while sync */
  float    level;         /* peak of decoded speech, 0..1, decays */
  uint64_t eoo_count;     /* End-of-Over frames seen */
  uint64_t dropped;       /* samples dropped at a ring boundary */
  int      squelch_open;  /* RX speech gate currently open */
  int      tx_finished;   /* EOO emitted after the last TX-off */
} rade_engine_stats_t;

/* Returns 0 on success, -1 if already running or RADE failed to open. */
int  rade_engine_start(void);
/* Stops the worker (joins) and frees everything.  Safe to call when stopped. */
void rade_engine_stop(void);
int  rade_engine_is_running(void);

/* ---- RX ---- */
/* Push 48 kHz mono receive audio (pre-AGC demod).  A block that does not fit
 * is dropped as a whole and counted. */
void rade_engine_push_rx(const float *mono48, int n);
/* Pop decoded 48 kHz speech; returns number of samples written (<= n). */
int  rade_engine_pop_rx(float *out, int n);
/* Free space in the receive ring (for file feeding / pacing). */
int  rade_engine_rx_free(void);
/* Drop receive state (sync, vocoder warm-up). */
void rade_engine_reset_rx(void);
/* Start/end speech gate (off by default). */
void rade_engine_set_squelch(int on, float open_snr_db);

/* ---- TX ---- */
void rade_engine_push_mic(const float *mic48, int n);
/* Pops modem audio, zero padding; returns number of real samples. */
int  rade_engine_pop_tx(float *out48, int n);
/* Enter/leave TX; leaving queues the End-of-Over frame. */
void rade_engine_set_tx(int on);
/* Convenience (Rust fill_tx): set_tx(mox && !unkeying), push mic, pop_tx
 * padded with zeros into out48.  Returns the number of real samples. */
int  rade_engine_tx_fill(int mox, const float *mic48, int n, float *out48);
/* While set, tx_fill forces TX off (EOO is sent) even if mox is still on. */
void rade_engine_set_unkeying(int on);
/* 1 once the EOO has been generated and every modulated sample was read. */
int  rade_engine_tx_drained(void);
/* Mic audio pushed but not yet modulated. */
int  rade_engine_tx_pending(void);
/* Callsign sent in the End-of-Over frame (empty clears). */
void rade_engine_set_callsign(const char *call);
/* Library's own TX band-pass filter. */
void rade_engine_set_tx_bpf(int enable);

/* ---- status / reports ---- */
void rade_engine_get_stats(rade_engine_stats_t *st);
/* Pops one callsign decoded from a remote End-of-Over frame (queue capped at
 * 50, oldest dropped).  Returns 1 and fills call/snr_db, 0 if none queued. */
int  rade_engine_next_callsign(char call[9], float *snr_db);

/* ---- optional mic conditioning (48 kHz, in place) ----
 * Order when enabled: denoiser -> leveler -> compressor -> EQ. */
void rade_engine_set_mic_denoise(int on);
void rade_engine_set_mic_leveler(int on);
void rade_engine_set_mic_compressor(int on);
void rade_engine_set_mic_eq(int on);
void rade_engine_set_mic_eq_params(float bass_hz, float bass_db,
                                   float mid_hz, float mid_db, float mid_q,
                                   float treble_hz, float treble_db,
                                   float vol_db);
void rade_engine_condition_mic(float *buf, int n);

#endif
