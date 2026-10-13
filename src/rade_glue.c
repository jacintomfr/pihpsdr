/* Copyright (C)
* RADE V1 integration glue for piHPSDR-IF, ported from the deskHPSDR
* Windows RADE port (CU2ED, docs/RADE-INTEGRATION.md there; see
* docs/RADE-IFPAN.md here for what differs).
*
* Runs on the server (the side that owns the radio): the RX tap and the TX
* source are in rx_process_buffer() / tx_add_mic_sample(), which only run
* there. A remote client's microphone reaches tx_add_mic_sample() as plain
* audio and the decoded speech goes back to it as ordinary RX audio.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifdef RADE

#include <glib.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "radio.h"
#include "receiver.h"
#include "transmitter.h"
#include "vfo.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "bandstack.h"
#include "ext.h"
#include "property.h"
#include "message.h"
#include "client_server.h"
#include "rade_engine.h"
#include "rade_glue.h"
#ifdef IFPAN
  #include "ifpan.h"
#endif

/*
 * Client / server: the engine runs on the server only. On a client the
 * menu's setters send CMD_RADE to the server and update the local copy at
 * once; the getters read that copy, which the server refreshes with
 * INFO_RADE every 150 ms (rade_glue_client_update). Incoming settings are
 * ignored for a short while after a local change, so a packet sent before
 * the server got the change does not flip a check box back.
 */
#define REMOTE_HOLD_US 600000
static gint64 remote_hold = 0;
static rade_engine_stats_t remote_stats;
static int remote_running = 0;

static int remote_send(int what, int b2, int value, const char *text) {
  if (!radio_is_remote) { return 0; }
  send_rade(cl_sock_tcp, what, b2, value, text);
  remote_hold = g_get_monotonic_time() + REMOTE_HOLD_US;
  return 1;
}

/* server: mode / filter / TX filter changed here -> the client's VFO bar,
 * filter and TX settings (piHPSDR only sends them for the client's own changes) */
static void push_client(void) {
#ifdef IFPAN
  ifpan_push_vfo_to_client();
#endif
}

#define RX_JITTER_CAP   48000   /* like rade_jitter in the Rust app */
#define RX_JITTER_PRIME 960     /* 20 ms before (re)starting playback */
#define TX_BLOCK        128     /* mic samples per engine call (2.7 ms) */
#define TX_GAIN         2.0     /* 1 / rade::TX_REAL_SCALE (0.5) */
#define UNKEY_TIMEOUT_MS 2000
#define UNKEY_SETTLE_MS  500

static GMutex engine_mutex;               /* engine start/stop vs audio threads */
static atomic_int rx_enabled = 0;
static atomic_int tx_armed = 0;
static atomic_int mute_analog = 0;
static atomic_int squelch_gate = 0;
static atomic_int denoiser = 0, leveler = 0, compressor = 0, eq = 0, filter_wide = 0;

/* Own callsign sent in the End-of-Over frame (piHPSDR has no "own callsign"
 * setting, so it lives here; edited in the RADE menu, saved as rade_callsign). */
static char rade_callsign[16] = "";

/* Mic EQ gains in tenths of a dB: bass 100 Hz, mid 1500 Hz (Q 1), treble 3000 Hz, output volume. */
static atomic_int eq_gain[4] = {0, 0, 0, 0};
#define EQ_RANGE_DB 20.0f

static void apply_eq_params(void) {   /* engine_mutex held, engine running */
  rade_engine_set_mic_eq_params(100.0f, atomic_load(&eq_gain[0]) / 10.0f,
                                1500.0f, atomic_load(&eq_gain[1]) / 10.0f, 1.0f,
                                3000.0f, atomic_load(&eq_gain[2]) / 10.0f,
                                atomic_load(&eq_gain[3]) / 10.0f);
}

/* ---------------- engine lifetime ---------------- */

static void engine_update(void) {
  int want = atomic_load(&rx_enabled) || atomic_load(&tx_armed);
  g_mutex_lock(&engine_mutex);
  if (want && !rade_engine_is_running()) {
    if (rade_engine_start() != 0) {
      t_print("%s: rade_engine_start failed\n", __func__);
      atomic_store(&rx_enabled, 0);
      atomic_store(&tx_armed, 0);
    } else {
      rade_engine_set_callsign(rade_callsign);
      rade_engine_set_mic_denoise(atomic_load(&denoiser));
      rade_engine_set_mic_leveler(atomic_load(&leveler));
      rade_engine_set_mic_compressor(atomic_load(&compressor));
      rade_engine_set_mic_eq(atomic_load(&eq));
      apply_eq_params();
      rade_engine_set_squelch(atomic_load(&squelch_gate), -4.0f);
    }
  } else if (!want && rade_engine_is_running()) {
    rade_engine_stop();
  }
  g_mutex_unlock(&engine_mutex);
}

/*
 * ---- WDSP TX chain forced to what the modem needs while armed (GTK thread) ----
 * piHPSDR: the leveler follows compressor/CFC (tx_set_compressor), the TX
 * filter is default_filter_low/high (or the RX filter with use_rx_filter),
 * and the setters store the values in the TX mode's profile, so forced
 * values sit in the DIGU/DIGL profile only while armed.
 */
#define RADE_TX_LOW  700
#define RADE_TX_HIGH 2300
static struct {
  int saved, compressor, eq_enable, cfc, cfc_eq, use_rx_filter, flow, fhigh;
} txs;

static void tx_restore_values(TRANSMITTER *tx) {
  txs.saved = 0;
  tx->compressor = txs.compressor;
  tx->eq_enable = txs.eq_enable;   tx->cfc = txs.cfc; tx->cfc_eq = txs.cfc_eq;
  tx->use_rx_filter = txs.use_rx_filter;
  tx->default_filter_low = txs.flow; tx->default_filter_high = txs.fhigh;
}

static void tx_chain_force(int on) {
  if (transmitter == NULL) { return; }
  TRANSMITTER *tx = transmitter;
  if (on && !txs.saved) {
    txs.saved = 1;
    txs.compressor = tx->compressor;
    txs.eq_enable = tx->eq_enable;   txs.cfc = tx->cfc; txs.cfc_eq = tx->cfc_eq;
    txs.use_rx_filter = tx->use_rx_filter;
    txs.flow = tx->default_filter_low; txs.fhigh = tx->default_filter_high;
    tx->compressor = 0; tx->eq_enable = 0; tx->cfc = 0; tx->cfc_eq = 0;
    tx->use_rx_filter = 0;
    tx->default_filter_low = RADE_TX_LOW; tx->default_filter_high = RADE_TX_HIGH;
  } else if (!on && txs.saved) {
    tx_restore_values(tx);
  } else {
    return;
  }
  tx_set_compressor(tx);
  tx_set_equalizer(tx);
  tx_set_filter(tx);
}

static int mode_is_digital(int m) { return m == modeDIGU || m == modeDIGL; }

/* RADE only works in DIGU/DIGL (the RX tap and the TX source are gated on it).
 * When the user switches RADE on or arms TX from the menu, move the VFO to the
 * digital mode of the band (DIGL below 10 MHz, DIGU above) instead of leaving
 * it in e.g. USB where RADE RX is silent and TX would send the plain voice.
 * piHPSDR-IF sends DIGU / DIGL to the Kenwood as USB / LSB ("Sync mode with
 * TS"), the modem audio going in through ACC2. */
/*
 * Where the operator was before RADE (piHPSDR-IF): switching RADE on takes a
 * snapshot of VFO A's band, band stack, frequency, CTUN, mode, filter and
 * deviation, the RX volume and the TX Drive (TS PWR, limited while RADE TX is
 * armed); switching RADE off puts all of it back, the Kenwood included,
 * whatever was tuned or changed meanwhile. Saved in the props file, so that
 * it also works after a restart with RADE on.
 */
static struct {
  int valid, id, band, bandstack, mode, filter, deviation, ctun;
  long long frequency, ctun_frequency;
  double volume, drive;
} snap;

static void snapshot_take(void) {
  if (active_receiver == NULL) { return; }
  int id = active_receiver->id;
  snap.valid = 1;
  snap.id = id;
  snap.band = vfo[id].band;
  snap.bandstack = vfo[id].bandstack;
  snap.mode = vfo[id].mode;
  snap.filter = vfo[id].filter;
  snap.deviation = vfo[id].deviation;
  snap.ctun = vfo[id].ctun;
  snap.frequency = vfo[id].frequency;
  snap.ctun_frequency = vfo[id].ctun_frequency;
  snap.volume = receiver[id]->volume;
  snap.drive = transmitter != NULL ? transmitter->drive : -1.0;
  t_print("%s: VFO %d band %d %lld Hz (ctun %d %lld) mode %d filter %d\n", __func__, id,
          snap.band, snap.frequency, snap.ctun, snap.ctun_frequency, snap.mode, snap.filter);
}

/* RADE only works in DIGU/DIGL (the RX tap and the TX source are gated on it).
 * When the user switches RADE on or arms TX from the menu, move the VFO to the
 * digital mode of the band (DIGL below 10 MHz, DIGU above) instead of leaving
 * it in e.g. USB where RADE RX is silent and TX would send the plain voice.
 * piHPSDR-IF sends DIGU / DIGL to the Kenwood as USB / LSB ("Sync mode with
 * TS"), the modem audio going in through ACC2. */
static void ensure_digital_mode(void) {
  if (active_receiver == NULL) { return; }
  int id = active_receiver->id;
  if (mode_is_digital(vfo[id].mode)) { return; }
  int m = (vfo[id].frequency < 10000000LL) ? modeDIGL : modeDIGU;
  t_print("%s: VFO %d was in mode %d, switching to %d for RADE\n", __func__, id, vfo[id].mode, m);
  vfo_id_mode_changed(id, m);
}

/* RADE switched off: everything back as it was before RADE (snapshot_take) */
static void snapshot_restore(void) {
  if (!snap.valid || active_receiver == NULL) { return; }
  snap.valid = 0;
  int id = snap.id;
  t_print("%s: VFO %d back to band %d %lld Hz (ctun %d %lld) mode %d filter %d\n", __func__, id,
          snap.band, snap.frequency, snap.ctun, snap.ctun_frequency, snap.mode, snap.filter);
  if (vfo[id].band != snap.band) { vfo_id_band_changed(id, snap.band); }
  vfo[id].bandstack = snap.bandstack;
  vfo[id].frequency = snap.frequency;
  vfo[id].ctun_frequency = snap.ctun_frequency;
  vfo[id].ctun = snap.ctun;
  vfo[id].deviation = snap.deviation;
  vfo_id_mode_changed(id, snap.mode);      /* loads the mode's profile */
  vfo_id_filter_changed(id, snap.filter);
  vfo_vfos_changed();
#ifdef IFPAN
  if (id == VFO_A) { ifpan_band_changed(snap.ctun ? snap.ctun_frequency : snap.frequency, snap.mode); }
#endif
  if (id < receivers) { radio_set_af_gain(id, snap.volume); }
  if (snap.drive >= 0.0) { radio_set_drive(snap.drive); }   /* TX is no longer armed: no RADE limit */
  g_idle_add(ext_vfo_update, NULL);
}

static void apply_fit_filter(void);
static void tune_band_rade_frequency(void);
static atomic_int tx_pref = 0;   /* "TX armed" as last chosen: armed again when RADE is entered */

static void drive_limit(void) {
  if (atomic_load(&tx_armed) && transmitter != NULL && transmitter->drive > RADE_DRIVE_MAX + 0.5) {
    radio_set_drive(RADE_DRIVE_MAX);   // TX Drive (TS PWR) limit while RADE TX is armed
  }
}

static void set_rx_enabled_int(int on, int force_mode) {
  int was_on = atomic_load(&rx_enabled) || atomic_load(&tx_armed);
  atomic_store(&rx_enabled, on ? 1 : 0);
  if (!on) { atomic_store(&tx_armed, 0); }
  int entering = on && force_mode && !was_on;
  if (entering) {
    snapshot_take();
    if (atomic_load(&tx_pref) && transmitter != NULL) { atomic_store(&tx_armed, 1); }
  }
  engine_update();
  if (entering && atomic_load(&rx_enabled)) { tune_band_rade_frequency(); }
  if (on && force_mode) { ensure_digital_mode(); }
  if (entering && atomic_load(&rx_enabled)) { apply_fit_filter(); }   /* the RADE RX filter (narrow / wide) */
  tx_chain_force(atomic_load(&tx_armed));
  drive_limit();
  if (on && force_mode && !atomic_load(&rx_enabled)) { snapshot_restore(); }   /* the engine did not start */
  if (!on && force_mode) { snapshot_restore(); }   /* after the TX values: the mode brings its own */
  if (force_mode) { push_client(); }   /* server-made mode / TX filter changes: tell the client */
  /* active_receiver is still NULL while the properties are restored at startup */
  t_print("%s: rx_enabled=%d tx_armed=%d\n", __func__, atomic_load(&rx_enabled), atomic_load(&tx_armed));
}
void rade_glue_set_rx_enabled(int on) {
  if (radio_is_remote) {
    atomic_store(&rx_enabled, on ? 1 : 0);
    if (!on) { atomic_store(&tx_armed, 0); }
    remote_send(RADE_REMOTE_RX, on ? 1 : 0, 0, NULL);
    return;
  }
  set_rx_enabled_int(on, 1);
}
int  rade_glue_get_rx_enabled(void) { return atomic_load(&rx_enabled); }
void rade_glue_set_tx_armed(int on) {
  int was_on = atomic_load(&rx_enabled) || atomic_load(&tx_armed);
  atomic_store(&tx_armed, on ? 1 : 0);
  if (on) { atomic_store(&rx_enabled, 1); }
  if (remote_send(RADE_REMOTE_TX, on ? 1 : 0, 0, NULL)) { return; }
  atomic_store(&tx_pref, on ? 1 : 0);
  if (on && !was_on) { snapshot_take(); }
  engine_update();
  if (on && !was_on && atomic_load(&rx_enabled)) { tune_band_rade_frequency(); }
  if (on) { ensure_digital_mode(); }
  if (on && !was_on && atomic_load(&rx_enabled)) { apply_fit_filter(); }
  tx_chain_force(atomic_load(&tx_armed));
  drive_limit();
  push_client();
  t_print("%s: tx_armed=%d tx mode=%d\n", __func__, atomic_load(&tx_armed), vfo_get_tx_mode());
}
void rade_glue_set_squelch(int on) {
  atomic_store(&squelch_gate, on ? 1 : 0);
  if (remote_send(RADE_REMOTE_SQUELCH, on ? 1 : 0, 0, NULL)) { return; }
  g_mutex_lock(&engine_mutex);
  if (rade_engine_is_running()) { rade_engine_set_squelch(on ? 1 : 0, -4.0f); }
  g_mutex_unlock(&engine_mutex);
}
int rade_glue_get_squelch(void) { return atomic_load(&squelch_gate); }
int  rade_glue_get_tx_armed(void) { return atomic_load(&tx_armed); }
int  rade_glue_engine_running(void) { return radio_is_remote ? remote_running : rade_engine_is_running(); }

void rade_glue_set_callsign(const char *call) {
  /* EOO alphabet is upper case; the codec maps what it cannot send */
  int n = 0;
  for (const char *p = call ? call : ""; *p && n < (int) sizeof(rade_callsign) - 1; p++) {
    if (*p != ' ') { rade_callsign[n++] = g_ascii_toupper(*p); }
  }
  rade_callsign[n] = 0;
  if (remote_send(RADE_REMOTE_CALLSIGN, 0, 0, rade_callsign)) { return; }
  g_mutex_lock(&engine_mutex);
  if (rade_engine_is_running()) { rade_engine_set_callsign(rade_callsign); }
  g_mutex_unlock(&engine_mutex);
}
const char *rade_glue_get_callsign_own(void) { return rade_callsign; }

void rade_glue_shutdown(void) {
  g_mutex_lock(&engine_mutex);
  if (txs.saved && transmitter != NULL) { tx_restore_values(transmitter); }  /* values only: radio_save_state() must store the user settings */
  rade_engine_stop();   /* flags are kept so radio_save_state() still stores them */
  g_mutex_unlock(&engine_mutex);
}

#define MIC_SETTER(name, var, call, remote) \
  void rade_glue_set_##name(int on) { \
    atomic_store(&var, on ? 1 : 0); \
    if (remote_send(remote, on ? 1 : 0, 0, NULL)) { return; } \
    g_mutex_lock(&engine_mutex); \
    if (rade_engine_is_running()) { call(on ? 1 : 0); } \
    g_mutex_unlock(&engine_mutex); \
  } \
  int rade_glue_get_##name(void) { return atomic_load(&var); }
MIC_SETTER(denoiser, denoiser, rade_engine_set_mic_denoise, RADE_REMOTE_DENOISER)
MIC_SETTER(leveler, leveler, rade_engine_set_mic_leveler, RADE_REMOTE_LEVELER)
MIC_SETTER(compressor, compressor, rade_engine_set_mic_compressor, RADE_REMOTE_COMPRESSOR)
MIC_SETTER(eq, eq, rade_engine_set_mic_eq, RADE_REMOTE_EQ)

void rade_glue_set_eq_gain(int band, float db) {
  if (band < 0 || band > 3) { return; }
  if (db > EQ_RANGE_DB) { db = EQ_RANGE_DB; }
  if (db < -EQ_RANGE_DB) { db = -EQ_RANGE_DB; }
  atomic_store(&eq_gain[band], (int) (db * 10.0f + (db < 0 ? -0.5f : 0.5f)));
  if (remote_send(RADE_REMOTE_EQ_GAIN, band, atomic_load(&eq_gain[band]), NULL)) { return; }
  g_mutex_lock(&engine_mutex);
  if (rade_engine_is_running()) { apply_eq_params(); }
  g_mutex_unlock(&engine_mutex);
}
float rade_glue_get_eq_gain(int band) { return (band < 0 || band > 3) ? 0.0f : atomic_load(&eq_gain[band]) / 10.0f; }

void rade_glue_set_mute_analog(int on) {
  atomic_store(&mute_analog, on ? 1 : 0);
  remote_send(RADE_REMOTE_MUTE, on ? 1 : 0, 0, NULL);
}
int  rade_glue_get_mute_analog(void) { return atomic_load(&mute_analog); }
void rade_glue_set_filter_wide(int on) { atomic_store(&filter_wide, on ? 1 : 0); }
int  rade_glue_get_filter_wide(void) { return atomic_load(&filter_wide); }

/* ---------------- RX ---------------- */

static float rx_jitter[RX_JITTER_CAP];
static int   rx_jlen = 0;
static int   rx_primed = 0;
static int   rx_was_active = 0;

void rade_glue_rx_block(int rx_id, double *stereo48, int frames) {
  if (rx_id != 0 || !atomic_load(&rx_enabled)) {
    rx_was_active = 0;
    return;
  }
  int m = vfo[rx_id].mode;
  static int last_inactive_mode = -1;
  if (!mode_is_digital(m)) {
    if (last_inactive_mode != m) {
      t_print("rade_glue: RX tap inactive, VFO mode %d is not DIGU/DIGL\n", m);
      last_inactive_mode = m;
    }
    rx_was_active = 0;
    return;
  }
  last_inactive_mode = -1;
  if (!g_mutex_trylock(&engine_mutex)) { return; }
  if (!rade_engine_is_running()) { g_mutex_unlock(&engine_mutex); return; }
  if (!rx_was_active) { rx_jlen = 0; rx_primed = 0; rx_was_active = 1; t_print("rade_glue: RX tap active (mode %d)\n", m); }
  float mono[frames];
  float speech[frames];
  for (int i = 0; i < frames; i++) {
    mono[i] = (float)(0.5 * (stereo48[2 * i] + stereo48[2 * i + 1]));
  }
  rade_engine_push_rx(mono, frames);
  int got = rade_engine_pop_rx(speech, frames);
  g_mutex_unlock(&engine_mutex);
  if (got > 0) {
    if (rx_jlen + got > RX_JITTER_CAP) {   /* drop oldest */
      int drop = rx_jlen + got - RX_JITTER_CAP;
      if (drop > rx_jlen) { drop = rx_jlen; }
      memmove(rx_jitter, rx_jitter + drop, (size_t)(rx_jlen - drop) * sizeof(float));
      rx_jlen -= drop;
    }
    memcpy(rx_jitter + rx_jlen, speech, (size_t)got * sizeof(float));
    rx_jlen += got;
  }
  if (!rx_primed && rx_jlen >= RX_JITTER_PRIME) { rx_primed = 1; }
  int mute = atomic_load(&mute_analog);
  int used = 0;
  for (int i = 0; i < frames; i++) {
    if (rx_primed && used < rx_jlen) {
      stereo48[2 * i] = stereo48[2 * i + 1] = (double) rx_jitter[used++];
    } else {
      rx_primed = 0;  /* underrun: re-prime */
      if (mute) { stereo48[2 * i] = stereo48[2 * i + 1] = 0.0; }
    }
  }
  if (used > 0) {
    memmove(rx_jitter, rx_jitter + used, (size_t)(rx_jlen - used) * sizeof(float));
    rx_jlen -= used;
  }
}

/* ---------------- TX ---------------- */

/* TX armed: the mic samples are taken over by the RADE hook in ANY mode.  Outside
 * DIGU/DIGL the hook sends silence, never the operator's voice. */
int rade_glue_tx_active(void) {
  return atomic_load_explicit(&tx_armed, memory_order_relaxed);
}

/* Armed AND in a digital mode: the modem is really being transmitted. */
static int tx_modem_mode(void) {
  return rade_glue_tx_active() && mode_is_digital(vfo_get_tx_mode());
}

static float tx_in[TX_BLOCK];
static float tx_out[TX_BLOCK];     /* modem audio of the previous block */
static int   tx_idx = 0;
static int   tx_was_active = 0;

double rade_glue_tx_sample(double mic_sample) {
  static long diag_n = 0;
  static int  diag_last_tx = -1;
  if (!tx_was_active) {
    tx_was_active = 1;
    tx_idx = 0;
    memset(tx_out, 0, sizeof(tx_out));
    t_print("rade_glue: TX hook active, tx mode %d\n", vfo_get_tx_mode());
  }
  int keyed = radio_is_transmitting();
  if (keyed != diag_last_tx) {
    diag_last_tx = keyed;
    diag_n = 0;
    t_print("rade_glue: transmitting=%d tx mode=%d digital=%d\n", keyed, vfo_get_tx_mode(),
            mode_is_digital(vfo_get_tx_mode()));
  }
  if (keyed && ++diag_n % 48000 == 0) {          /* once per second while keyed */
    rade_engine_stats_t st;
    rade_engine_get_stats(&st);
    t_print("rade_glue: TX 1s: pending_out=%d dropped=%llu tx_finished=%d\n",
            rade_engine_tx_pending(), (unsigned long long) st.dropped, st.tx_finished);
  }
  if (!mode_is_digital(vfo_get_tx_mode())) {
    return 0.0;                                   /* never send the plain voice */
  }
  double ret = tx_out[tx_idx];
  tx_in[tx_idx++] = (float) mic_sample;
  if (tx_idx >= TX_BLOCK) {
    tx_idx = 0;
    if (g_mutex_trylock(&engine_mutex)) {
      if (rade_engine_is_running()) {
        rade_engine_condition_mic(tx_in, TX_BLOCK);
        rade_engine_tx_fill(radio_is_transmitting(), tx_in, TX_BLOCK, tx_out);
        for (int i = 0; i < TX_BLOCK; i++) {
          float s = tx_out[i] * (float) TX_GAIN;
          tx_out[i] = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        }
      } else {
        memset(tx_out, 0, sizeof(tx_out));
      }
      g_mutex_unlock(&engine_mutex);
    } else {
      memset(tx_out, 0, sizeof(tx_out));
    }
  }
  return ret;
}

/* ---------------- over end / unkeying ---------------- */

static atomic_uint unkey_gen = 0;
static atomic_int  unkey_pending = 0;
static int         unkey_bypass = 0;      /* GTK thread only */
static gint64      unkey_t0, unkey_drained_at;

static gboolean unkey_poll(gpointer data) {
  if ((guint) GPOINTER_TO_UINT(data) != atomic_load(&unkey_gen)) { return G_SOURCE_REMOVE; }
  gint64 now = g_get_monotonic_time();
  if (unkey_drained_at == 0 && rade_engine_tx_drained()) { unkey_drained_at = now; }
  int settled = unkey_drained_at != 0 && (now - unkey_drained_at) >= UNKEY_SETTLE_MS * 1000;
  int timeout = (now - unkey_t0) > UNKEY_TIMEOUT_MS * 1000;
  if (settled || timeout || !atomic_load(&tx_armed)) {
    t_print("%s: PTT released %lld ms after unkey request (timeout=%d)\n", __func__,
            (long long)((now - unkey_t0) / 1000), !settled);
    atomic_store(&unkey_pending, 0);
    unkey_bypass = 1;
    radio_set_mox(0);
    unkey_bypass = 0;
    rade_engine_set_unkeying(0);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

int rade_glue_mox_off_intercept(void) {
  if (unkey_bypass) { return 0; }
  if (atomic_load(&unkey_pending)) { return 1; }
  if (!tx_modem_mode() || !rade_engine_is_running() || !mox) { return 0; }
  atomic_store(&unkey_pending, 1);
  unkey_t0 = g_get_monotonic_time();
  unkey_drained_at = 0;
  guint gen = atomic_fetch_add(&unkey_gen, 1) + 1;
  rade_engine_set_unkeying(1);
  g_timeout_add(20, unkey_poll, GUINT_TO_POINTER(gen));
  return 1;
}

void rade_glue_mox_on(void) {
  if (atomic_load(&unkey_pending)) {
    atomic_fetch_add(&unkey_gen, 1);   /* invalidates the poll */
    atomic_store(&unkey_pending, 0);
    rade_engine_set_unkeying(0);
  }
}

/* ---------------- stats / callsigns ---------------- */

static GMutex call_mutex;
static char  call_hist[RADE_GLUE_HISTORY][9];
static float call_snr[RADE_GLUE_HISTORY];
static int   call_n = 0;

static void call_pump(void) {
  char c[9];
  float snr;
  if (radio_is_remote) { return; }    // the client's history comes from the server
  g_mutex_lock(&call_mutex);
  while (rade_engine_next_callsign(c, &snr)) {
    memmove(call_hist[1], call_hist[0], sizeof(call_hist[0]) * (RADE_GLUE_HISTORY - 1));
    memmove(&call_snr[1], &call_snr[0], sizeof(float) * (RADE_GLUE_HISTORY - 1));
    memcpy(call_hist[0], c, 9);
    call_snr[0] = snr;
    if (call_n < RADE_GLUE_HISTORY) { call_n++; }
  }
  g_mutex_unlock(&call_mutex);
}

void rade_glue_get_stats(rade_engine_stats_t *st) {
  if (radio_is_remote) { *st = remote_stats; return; }
  rade_engine_get_stats(st);
}

/* ---------------- Quick Tune / Fit Filter (run on the server) ---------------- */

static const struct { const char *label; long long hz; } quick_tune[] = {
  {"80m", 3630000LL}, {"40m", 7180000LL}, {"20m", 14236000LL},
  {"15m", 21180000LL}, {"10m", 28330000LL}
};
#define N_QUICK_TUNE ((int)(sizeof(quick_tune) / sizeof(quick_tune[0])))
static int fit_applied = 0;   /* Var1 filter was set by Fit Filter: follow mode changes */

int rade_glue_quick_tune_count(void) { return N_QUICK_TUNE; }
const char *rade_glue_quick_tune_label(int i, long long *hz) {
  if (i < 0 || i >= N_QUICK_TUNE) { return ""; }
  if (hz) { *hz = quick_tune[i].hz; }
  return quick_tune[i].label;
}

/*
 * Set the Var1 filter of the active receiver's VFO to the RADE passband.
 * Var1 edges are in the CAT ZZFL/ZZFH convention: (-high, -low) for the
 * lower sideband mode DIGL.  The TX bandpass is forced to 700-2300 Hz by
 * the glue while TX is armed, so it is not touched here.
 */
static void apply_fit_filter(void) {
  if (active_receiver == NULL) { return; }
  int id = active_receiver->id;
  int mode = vfo[id].mode;
  if (!mode_is_digital(mode)) { return; }
  int low = atomic_load(&filter_wide) ? 300 : 700;
  int high = atomic_load(&filter_wide) ? 2700 : 2300;
  FILTER *f = &filters[mode][filterVar1];
  if (mode == modeDIGL) {
    f->low = -high;
    f->high = -low;
  } else {
    f->low = low;
    f->high = high;
  }
  vfo_id_filter_changed(id, filterVar1);
  fit_applied = 1;
  push_client();
  g_idle_add(ext_vfo_update, NULL);
}

// rotate wide <-> narrow (the menu checks the mode first)
void rade_glue_fit_filter(void) {
  atomic_store(&filter_wide, !atomic_load(&filter_wide));
  if (remote_send(RADE_REMOTE_FIT_FILTER, 0, 0, NULL)) { return; }
  apply_fit_filter();
}

/* receiver, VFO and Kenwood to a RADE frequency, in DIGL below 10 MHz, DIGU above */
static void rade_tune(int id, long long hz) {
  // lower sideband below 10 MHz (digi_mode_for_band of the Rust app)
  int mode = (hz < 10000000LL) ? modeDIGL : modeDIGU;
  if (vfo[id].mode != mode) {
    vfo_id_mode_changed(id, mode);
  }
#ifdef IFPAN
  //
  // The receiver (CTUN) goes there at once, as with a band change: the
  // Kenwood moving alone would leave the receiver where it was whenever the
  // new frequency is still inside the span (14.210 -> 14.236 at 96 kHz).
  // Then the Kenwood is tuned (frequency and USB / LSB), the display follows.
  //
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
}

/* entering RADE: straight to the band's RADE frequency, if the band has one */
static void tune_band_rade_frequency(void) {
  if (active_receiver == NULL) { return; }
  int id = active_receiver->id;
  for (int i = 0; i < N_QUICK_TUNE; i++) {
    if (get_band_from_frequency(quick_tune[i].hz) == vfo[id].band) {
      t_print("%s: band %d: RADE frequency %lld Hz\n", __func__, vfo[id].band, quick_tune[i].hz);
      rade_tune(id, quick_tune[i].hz);
      return;
    }
  }
}

void rade_glue_quick_tune(int i) {
  if (i < 0 || i >= N_QUICK_TUNE) { return; }
  if (remote_send(RADE_REMOTE_QUICK_TUNE, i, 0, NULL)) { return; }
  if (active_receiver == NULL) { return; }
  rade_tune(active_receiver->id, quick_tune[i].hz);
  if (fit_applied) { apply_fit_filter(); }   // keep the fitted width, sign follows the mode
  push_client();
  g_idle_add(ext_vfo_update, NULL);
}

/* ---------------- client / server ---------------- */

// server: CMD_RADE from the client's RADE menu (GTK thread)
void rade_glue_remote_command(int what, int b2, int value, const char *text) {
  switch (what) {
  case RADE_REMOTE_RX:         rade_glue_set_rx_enabled(b2); break;
  case RADE_REMOTE_TX:         rade_glue_set_tx_armed(b2); break;
  case RADE_REMOTE_DENOISER:   rade_glue_set_denoiser(b2); break;
  case RADE_REMOTE_LEVELER:    rade_glue_set_leveler(b2); break;
  case RADE_REMOTE_COMPRESSOR: rade_glue_set_compressor(b2); break;
  case RADE_REMOTE_EQ:         rade_glue_set_eq(b2); break;
  case RADE_REMOTE_MUTE:       rade_glue_set_mute_analog(b2); break;
  case RADE_REMOTE_SQUELCH:    rade_glue_set_squelch(b2); break;
  case RADE_REMOTE_EQ_GAIN:    rade_glue_set_eq_gain(b2, value / 10.0f); break;
  case RADE_REMOTE_FIT_FILTER: rade_glue_fit_filter(); break;
  case RADE_REMOTE_QUICK_TUNE: rade_glue_quick_tune(b2); break;
  case RADE_REMOTE_CALLSIGN: {
    char call[17];
    memcpy(call, text, 16);           // not necessarily terminated on the wire
    call[16] = 0;
    rade_glue_set_callsign(call);
  }
  break;
  default:
    t_print("%s: unknown RADE command %d\n", __func__, what);
    break;
  }
}

static uint16_t to_s16(float v) {
  long i = lroundf(v);
  if (i > 32767) { i = 32767; }
  if (i < -32768) { i = -32768; }
  return to_16((uint16_t)(int16_t) i);
}

// server: state and settings for the client (with the periodic data, GTK thread)
void rade_glue_fill_remote(RADE_DATA *d) {
  memset(d, 0, sizeof(*d));
  int running = rade_engine_is_running();
  int flags = (atomic_load(&rx_enabled) ? RADE_FLAG_RX : 0)
              | (atomic_load(&tx_armed) ? RADE_FLAG_TX : 0)
              | (atomic_load(&denoiser) ? RADE_FLAG_DENOISER : 0)
              | (atomic_load(&leveler) ? RADE_FLAG_LEVELER : 0)
              | (atomic_load(&compressor) ? RADE_FLAG_COMPR : 0)
              | (atomic_load(&eq) ? RADE_FLAG_EQ : 0)
              | (atomic_load(&mute_analog) ? RADE_FLAG_MUTE : 0)
              | (atomic_load(&squelch_gate) ? RADE_FLAG_SQUELCH : 0)
              | (atomic_load(&filter_wide) ? RADE_FLAG_WIDE : 0)
              | (running ? RADE_FLAG_RUNNING : 0);
  d->flags = to_16(flags);
  if (running) {
    rade_engine_stats_t st;
    rade_engine_get_stats(&st);
    d->eoo_count = to_32((int) st.eoo_count);
    d->dropped = to_32((int) st.dropped);
    d->snr = to_s16(10.0f * st.snr_db);
    d->foff = to_s16(10.0f * st.foff_hz);
    d->sync = st.sync ? 1 : 0;
    float lv = st.level < 0.0f ? 0.0f : (st.level > 1.0f ? 1.0f : st.level);
    d->level = (uint8_t) lroundf(255.0f * lv);
  }
  for (int i = 0; i < 4; i++) { d->eq_gain[i] = to_16((uint16_t)(int16_t) atomic_load(&eq_gain[i])); }
  g_strlcpy(d->callsign, rade_callsign, sizeof(d->callsign));
  call_pump();
  g_mutex_lock(&call_mutex);
  int n = call_n < RADE_REMOTE_CALLS ? call_n : RADE_REMOTE_CALLS;
  d->call_n = n;
  for (int i = 0; i < n; i++) {
    memcpy(d->calls[i], call_hist[i], 9);
    d->call_snr[i] = to_s16(10.0f * call_snr[i]);
  }
  g_mutex_unlock(&call_mutex);
}

// client: the server's state (INFO_RADE, GTK thread)
void rade_glue_client_update(const RADE_DATA *d) {
  int flags = from_16(d->flags);
  remote_running = (flags & RADE_FLAG_RUNNING) != 0;
  remote_stats.sync = d->sync;
  remote_stats.snr_db = (int16_t) from_16(d->snr) / 10.0f;
  remote_stats.foff_hz = (int16_t) from_16(d->foff) / 10.0f;
  remote_stats.level = d->level / 255.0f;
  remote_stats.eoo_count = (uint32_t) from_32(d->eoo_count);
  remote_stats.dropped = (uint32_t) from_32(d->dropped);
  g_mutex_lock(&call_mutex);
  call_n = d->call_n < RADE_GLUE_HISTORY ? d->call_n : RADE_GLUE_HISTORY;
  for (int i = 0; i < call_n; i++) {
    memcpy(call_hist[i], d->calls[i], 9);
    call_hist[i][8] = 0;
    call_snr[i] = (int16_t) from_16(d->call_snr[i]) / 10.0f;
  }
  g_mutex_unlock(&call_mutex);
  if (g_get_monotonic_time() < remote_hold) { return; }   // a local change is on its way
  atomic_store(&rx_enabled, (flags & RADE_FLAG_RX) != 0);
  atomic_store(&tx_armed, (flags & RADE_FLAG_TX) != 0);
  atomic_store(&denoiser, (flags & RADE_FLAG_DENOISER) != 0);
  atomic_store(&leveler, (flags & RADE_FLAG_LEVELER) != 0);
  atomic_store(&compressor, (flags & RADE_FLAG_COMPR) != 0);
  atomic_store(&eq, (flags & RADE_FLAG_EQ) != 0);
  atomic_store(&mute_analog, (flags & RADE_FLAG_MUTE) != 0);
  atomic_store(&squelch_gate, (flags & RADE_FLAG_SQUELCH) != 0);
  atomic_store(&filter_wide, (flags & RADE_FLAG_WIDE) != 0);
  for (int i = 0; i < 4; i++) { atomic_store(&eq_gain[i], (int16_t) from_16(d->eq_gain[i])); }
  char call[17];
  memcpy(call, d->callsign, 16);
  call[16] = 0;
  g_strlcpy(rade_callsign, call, sizeof(rade_callsign));
}

int rade_glue_callsign_count(void) { call_pump(); return call_n; }

int rade_glue_get_callsign(int idx, char call[9], float *snr_db) {
  call_pump();
  int ok = 0;
  g_mutex_lock(&call_mutex);
  if (idx >= 0 && idx < call_n) {
    memcpy(call, call_hist[idx], 9);
    if (snr_db) { *snr_db = call_snr[idx]; }
    ok = 1;
  }
  g_mutex_unlock(&call_mutex);
  return ok;
}

/* ---------------- persistence ---------------- */
/* Caller (radio.c, server part of radio_restore_state / radio_save_state)
 * holds property_mutex.  Runs before receivers / VFOs / the transmitter
 * exist: nothing reachable from here may touch them. */

/* startup after a program end in RADE (main thread): back to before RADE */
static gboolean startup_restore(gpointer data) {
  static int tries = 0;
  (void)data;
  if (!snap.valid) { return G_SOURCE_REMOVE; }
  int ready = active_receiver != NULL;
#ifdef IFPAN
  ready = ready && ifpan_cat_connected;
#endif
  if (!ready && ++tries < 60) { return G_SOURCE_CONTINUE; }   /* up to 30 s */
  if (active_receiver == NULL) { return G_SOURCE_REMOVE; }
  t_print("%s: the program ended in RADE: back to before RADE\n", __func__);
  snapshot_restore();
  push_client();
  return G_SOURCE_REMOVE;
}

void rade_glue_load_state(void) {
  int v;
  v = atomic_load(&denoiser);    GetPropI0("rade_denoiser", v);    atomic_store(&denoiser, v);
  v = atomic_load(&leveler);     GetPropI0("rade_leveler", v);     atomic_store(&leveler, v);
  v = atomic_load(&compressor);  GetPropI0("rade_compressor", v);  atomic_store(&compressor, v);
  v = atomic_load(&eq);          GetPropI0("rade_eq", v);          atomic_store(&eq, v);
  v = atomic_load(&mute_analog); GetPropI0("rade_mute_analog", v); atomic_store(&mute_analog, v);
  v = atomic_load(&filter_wide); GetPropI0("rade_filter_wide", v); atomic_store(&filter_wide, v);
  for (int i = 0; i < 4; i++) {
    char key[24];
    snprintf(key, sizeof(key), "rade_eq_gain%d", i);
    v = atomic_load(&eq_gain[i]); GetPropI0(key, v); atomic_store(&eq_gain[i], v);
  }
  GetPropS0("rade_callsign", rade_callsign);
  v = atomic_load(&tx_pref);     GetPropI0("rade_tx_pref", v);     atomic_store(&tx_pref, v);
  /*
   * RADE is entered by opening the RADE menu and left with its Close button,
   * so it is never switched on at startup. If the program was ended in RADE,
   * where the operator was before (snapshot_take) is put back once the radio
   * (and the Kenwood's CAT) is up.
   */
  v = 0;                         GetPropI0("rade_prev.valid", v);
  if (v) {
    snap.valid = 1;
    GetPropI0("rade_prev.id", snap.id);
    GetPropI0("rade_prev.band", snap.band);
    GetPropI0("rade_prev.bandstack", snap.bandstack);
    GetPropI0("rade_prev.mode", snap.mode);
    GetPropI0("rade_prev.filter", snap.filter);
    GetPropI0("rade_prev.deviation", snap.deviation);
    GetPropI0("rade_prev.ctun", snap.ctun);
    GetPropI0("rade_prev.frequency", snap.frequency);
    GetPropI0("rade_prev.ctun_frequency", snap.ctun_frequency);
    GetPropF0("rade_prev.volume", snap.volume);
    GetPropF0("rade_prev.drive", snap.drive);
    g_timeout_add(500, startup_restore, NULL);
  }
}

void rade_glue_save_state(void) {
  SetPropI0("rade_enabled",     atomic_load(&rx_enabled));
  SetPropI0("rade_denoiser",    atomic_load(&denoiser));
  SetPropI0("rade_leveler",     atomic_load(&leveler));
  SetPropI0("rade_compressor",  atomic_load(&compressor));
  SetPropI0("rade_eq",          atomic_load(&eq));
  SetPropI0("rade_mute_analog", atomic_load(&mute_analog));
  SetPropI0("rade_filter_wide", atomic_load(&filter_wide));
  for (int i = 0; i < 4; i++) {
    char key[24];
    snprintf(key, sizeof(key), "rade_eq_gain%d", i);
    SetPropI0(key, atomic_load(&eq_gain[i]));
  }
  SetPropS0("rade_callsign",    rade_callsign);
  SetPropI0("rade_tx_pref",     atomic_load(&tx_pref));
  int on = snap.valid;
  SetPropI0("rade_prev.valid", on);
  if (on) {
    SetPropI0("rade_prev.id", snap.id);
    SetPropI0("rade_prev.band", snap.band);
    SetPropI0("rade_prev.bandstack", snap.bandstack);
    SetPropI0("rade_prev.mode", snap.mode);
    SetPropI0("rade_prev.filter", snap.filter);
    SetPropI0("rade_prev.deviation", snap.deviation);
    SetPropI0("rade_prev.ctun", snap.ctun);
    SetPropI0("rade_prev.frequency", snap.frequency);
    SetPropI0("rade_prev.ctun_frequency", snap.ctun_frequency);
    SetPropF0("rade_prev.volume", snap.volume);
    SetPropF0("rade_prev.drive", snap.drive);
  }
}

#endif /* RADE */
