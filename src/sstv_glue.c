/* SSTV for piHPSDR-IF: glue between piHPSDR and sstv-lib.
 *
 * Phase 1 (receive, server screen): RX1's demodulated audio (the same tap as
 * RADE, but ahead of it) goes to the SSTV decoder while the SSTV window is
 * open - as in hpsdr-rs, no decoding with nobody looking (sstv_link.rs).
 * Completed pictures are saved as PNG in sstv_images/ next to the settings
 * (hpsdr-rs: auto-save on by default).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef SSTV

#include <glib.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>

#include "band.h"
#include "bandstack.h"
#include "ext.h"
#include "filter.h"
#include "message.h"
#include "mode.h"
#include "property.h"
#include "radio.h"
#include "receiver.h"
#include "sstv_glue.h"
#include "vfo.h"
#ifdef IFPAN
  #include "ifpan.h"
#endif

#define SSTV_BLOCK 1024
#define SSTV_IMAGE_DIR "sstv_images"

static sstv_t    *engine = NULL;
//
// Diagnosis (opt-in): with ~/sstv-dump.on present when SSTV starts, the decoder
// input (mono float, 48 kHz) goes to /tmp/sstv_in.f32, at most 5 minutes, for
// replaying through sstv.rs offline. Opened once, never closed under the
// audio thread (only flushed).
//
static FILE      *dbg_dump = NULL;
static long       dbg_dump_n = 0;
static GMutex     engine_mutex;
static atomic_int rx_enabled = 0;
static int        sel_mode = -1;      // -1: Auto
static int        auto_save = 1;

static sstv_t *get_engine(void) {
  g_mutex_lock(&engine_mutex);
  if (engine == NULL) {
    engine = sstv_new();
    sstv_set_expected(engine, sel_mode);
    sstv_set_auto_save(engine, auto_save ? SSTV_IMAGE_DIR : NULL);
    t_print("%s: SSTV decoder ready (%d modes)\n", __func__, sstv_mode_count());
  }
  g_mutex_unlock(&engine_mutex);
  return engine;
}

void sstv_glue_rx_block(int rx_id, const double *stereo48, int frames) {
  if (rx_id != 0 || !atomic_load(&rx_enabled) || engine == NULL) { return; }
  //
  // The tap is after WDSP's AF gain (the AF slider, SetRXAPanelGain1): undo
  // it, so the decoder sees the level it would at AF 0 dB whatever the
  // listening volume (hpsdr-rs taps ahead of its Audio Gain). At the slider's
  // bottom (-39.5 dB and below) WDSP sends silence: nothing to undo.
  //
  double gain = 1.0;
  if (receivers > 0 && receiver[0] != NULL) {
    double v = receiver[0]->volume;
    if (v <= -39.5) { return; }
    if (v < 0.0) { gain = pow(10.0, -0.05 * v); }
  }
  float buf[SSTV_BLOCK];
  int done = 0;
  while (done < frames) {
    int n = frames - done;
    if (n > SSTV_BLOCK) { n = SSTV_BLOCK; }
    for (int i = 0; i < n; i++) {
      // mono downmix, as hpsdr-rs spectrum.rs feeds sstv_scratch
      buf[i] = (float)(0.5 * gain * (stereo48[2 * (done + i)] + stereo48[2 * (done + i) + 1]));
    }
    sstv_feed_rx(engine, buf, (size_t) n);
    if (dbg_dump != NULL && dbg_dump_n < 48000L * 300) {
      fwrite(buf, sizeof(float), (size_t) n, dbg_dump);
      dbg_dump_n += n;
    }
    done += n;
  }
}

//
// While SSTV decodes, the receiver squelch is off (a closed squelch feeds the
// decoder silence: the weak parts of a picture would be lost); it is put
// back as it was when the SSTV panel closes.
//
static int squelch_was_on = 0;

void sstv_glue_set_rx_enabled(int on) {
  if (on) { get_engine(); }
  if (on && dbg_dump == NULL) {
    char flag[512];
    snprintf(flag, sizeof(flag), "%s/sstv-dump.on", g_get_home_dir());
    FILE *t = fopen(flag, "r");
    if (t != NULL) {
      fclose(t);
      dbg_dump_n = 0;
      dbg_dump = fopen("/tmp/sstv_in.f32", "wb");
      t_print("%s: recording the decoder input to /tmp/sstv_in.f32 (%s)\n", __func__, flag);
    }
  } else if (!on && dbg_dump != NULL) {
    fflush(dbg_dump);
  }
  if (!radio_is_remote && receivers > 0 && receiver[0] != NULL) {
    if (on && !atomic_load(&rx_enabled) && receiver[0]->squelch_enable) {
      squelch_was_on = 1;
      radio_set_squelch_enable(0, 0);
      t_print("%s: squelch off while SSTV decodes\n", __func__);
    } else if (!on && atomic_load(&rx_enabled) && squelch_was_on) {
      squelch_was_on = 0;
      radio_set_squelch_enable(0, 1);
      t_print("%s: squelch back on\n", __func__);
    }
  }
  atomic_store(&rx_enabled, on ? 1 : 0);
  t_print("%s: SSTV RX %s\n", __func__, on ? "on" : "off");
}

const sstv_t *sstv_glue_engine(void) {
  return engine;
}

void sstv_glue_set_mode(int mode) {
  sel_mode = (mode >= 0 && mode < sstv_mode_count()) ? mode : -1;
  if (engine != NULL) { sstv_set_expected(engine, sel_mode); }
}

int sstv_glue_get_mode(void) {
  return sel_mode;
}

void sstv_glue_set_auto_save(int on) {
  auto_save = on ? 1 : 0;
  if (engine != NULL) { sstv_set_auto_save(engine, auto_save ? SSTV_IMAGE_DIR : NULL); }
}

int sstv_glue_get_auto_save(void) {
  return auto_save;
}

const char *sstv_glue_image_dir(void) {
  return SSTV_IMAGE_DIR;
}

//
// hpsdr-rs's SSTV Fit Filter: the RX passband to the SSTV tone band, sync
// 1200 Hz through white 2300 Hz, plus a 100 Hz margin (1100 - 2400 Hz,
// negative on the lower sideband), in the mode's Var1 filter.
//
void sstv_glue_fit_filter(void) {
  if (radio_is_remote || active_receiver == NULL) { return; }
  int id = active_receiver->id;
  int mode = vfo[id].mode;
  if (mode != modeUSB && mode != modeLSB && mode != modeDIGU && mode != modeDIGL) { return; }
  FILTER *f = &filters[mode][filterVar1];
  if (mode == modeLSB || mode == modeDIGL) {
    f->low = -2400;
    f->high = -1100;
  } else {
    f->low = 1100;
    f->high = 2400;
  }
  vfo_id_filter_changed(id, filterVar1);
#ifdef IFPAN
  ifpan_push_vfo_to_client();
#endif
  g_idle_add(ext_vfo_update, NULL);
}

//
// the filter from before SSTV: Fit Filter (and Quick Tune) rewrite Var1 of
// USB / LSB / DIGU / DIGL and select it; on close the edges and RX1's filter
// come back (the filter only if the mode is still the one from before)
//
static const int fs_modes[4] = { modeUSB, modeLSB, modeDIGU, modeDIGL };
static int fs_low[4], fs_high[4];
static int fs_mode = -1, fs_filter = -1;

void sstv_glue_filter_take(void) {
  if (radio_is_remote) { return; }
  for (int i = 0; i < 4; i++) {
    fs_low[i]  = filters[fs_modes[i]][filterVar1].low;
    fs_high[i] = filters[fs_modes[i]][filterVar1].high;
  }
  fs_mode   = vfo[0].mode;
  fs_filter = vfo[0].filter;
}

void sstv_glue_filter_restore(void) {
  if (radio_is_remote || fs_mode < 0) { return; }
  for (int i = 0; i < 4; i++) {
    filters[fs_modes[i]][filterVar1].low  = fs_low[i];
    filters[fs_modes[i]][filterVar1].high = fs_high[i];
  }
  if (vfo[0].mode == fs_mode) {
    vfo_id_filter_changed(0, fs_filter);
  } else if (vfo[0].filter == filterVar1) {
    vfo_id_filter_changed(0, filterVar1);   // Var1 of this mode has its old edges again
  }
  fs_mode = fs_filter = -1;
#ifdef IFPAN
  ifpan_push_vfo_to_client();
#endif
  g_idle_add(ext_vfo_update, NULL);
}

//
// hpsdr-rs's SSTV Quick Tune: a calling frequency, DIGL below 10 MHz and
// DIGU above (digi_mode_for_band), then Fit Filter. As RADE's Quick Tune:
// receiver, VFO and Kenwood together (rade_glue.c rade_tune).
//
void sstv_glue_quick_tune(long long hz) {
  if (radio_is_remote || active_receiver == NULL) { return; }
  int id = active_receiver->id;
  int mode = (hz < 10000000LL) ? modeDIGL : modeDIGU;
  if (vfo[id].mode != mode) { vfo_id_mode_changed(id, mode); }
#ifdef IFPAN
  int b = get_band_from_frequency(hz);
  if (b >= 0 && b != vfo[id].band) {
    vfo[id].band = b;
    vfo[id].bandstack = bandstack_get_bandstack(b)->current_entry;
  }
  vfo[id].frequency = hz;
  vfo[id].ctun_frequency = hz;
  vfo_vfos_changed();
  ifpan_band_changed(hz, mode);
#else
  vfo_id_set_frequency(id, hz);
#endif
  sstv_glue_fit_filter();
}

void sstv_glue_load_state(void) {
  GetPropI0("sstv.mode",      sel_mode);
  GetPropI0("sstv.auto_save", auto_save);
}

void sstv_glue_save_state(void) {
  SetPropI0("sstv.mode",      sel_mode);
  SetPropI0("sstv.auto_save", auto_save);
}

#endif /* SSTV */
