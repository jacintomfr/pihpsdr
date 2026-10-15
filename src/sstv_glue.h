/* SSTV for piHPSDR-IF: glue between piHPSDR and sstv-lib (phase 1: receive
 * on the server's own screen).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _SSTV_GLUE_H
#define _SSTV_GLUE_H

#ifdef SSTV

#include "sstv_lib.h"

// receiver.c: RX1 demodulated audio (48 kHz stereo, before RADE)
void        sstv_glue_rx_block(int rx_id, const double *stereo48, int frames);

void        sstv_glue_set_rx_enabled(int on);    // decoding only while the SSTV window is open
const sstv_t *sstv_glue_engine(void);            // NULL before the first use
void        sstv_glue_set_mode(int mode);        // -1: Auto
int         sstv_glue_get_mode(void);
void        sstv_glue_set_auto_save(int on);
int         sstv_glue_get_auto_save(void);
const char *sstv_glue_image_dir(void);
void        sstv_glue_fit_filter(void);           // RX passband 1100 - 2400 Hz
void        sstv_glue_filter_take(void);          // panel open: remember RX1's filter and the Var1 edges
void        sstv_glue_filter_restore(void);       // panel close: put them back
void        sstv_glue_quick_tune(long long hz);   // calling frequency, DIGL/DIGU, Fit Filter

void        sstv_glue_load_state(void);
void        sstv_glue_save_state(void);

#endif
#endif
