/* SSTV modem for piHPSDR-IF: C interface of sstv-lib (Rust static library,
 * hpsdr-rs's sstv.rs from SDRoxide, see sstv-lib/src/lib.rs).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _SSTV_LIB_H
#define _SSTV_LIB_H

#include <stddef.h>
#include <stdint.h>

typedef struct Sstv sstv_t;

typedef struct {
  uint32_t image_id;      // changes with every new picture (or restart)
  uint16_t w, h;          // picture size, 0 before the first one
  int      receiving;     // 1 while a picture is being decoded
  float    progress;      // 0..1 of the current picture
  int      detected;      // index for sstv_mode_label(), -1: none
  float    level;         // smoothed in-band level, ~0..1
  float    sync_quality;  // last line's sync pulse, 0..1 (tuning aid)
  uint32_t saved_count;   // pictures auto-saved so far
  char     unsupported[32];  // last header of a mode this decoder cannot draw
  char     rx_id[32];        // callsign from the last FSK ID
  char     last_saved[256];  // path of the last saved picture
} sstv_status_t;

sstv_t     *sstv_new(void);
void        sstv_free(sstv_t *s);
void        sstv_feed_rx(const sstv_t *s, const float *audio, size_t n);   // mono, 48 kHz
void        sstv_restart(const sstv_t *s);
void        sstv_set_expected(const sstv_t *s, int mode);                 // -1: Auto
void        sstv_set_auto_save(const sstv_t *s, const char *dir);         // NULL: off
int         sstv_mode_count(void);
const char *sstv_mode_label(int i);
void        sstv_get_status(const sstv_t *s, sstv_status_t *st);
size_t      sstv_copy_rgb(const sstv_t *s, uint8_t *dst, size_t cap);

#endif
