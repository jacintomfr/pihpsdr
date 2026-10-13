/* Copyright (C)
*  2026 - CU2ED
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

//
// IF panadapter: CAT tracking of the Kenwood TS-570D through Hamlib's
// rigctld (which owns the serial port), e.g.
//
//   rigctld -m 2004 -r /dev/ttyUSB0 -s 57600
//
// (TS-570D menu 35 = "576-1"; the radio needs a power cycle after changing
// it). A thread polls the VFO frequency ("f") and, when it changes, hands it
// to the GTK main loop which moves the display centre. The PlutoSDR LO
// is never touched, so the RX stream is not interrupted while the
// Kenwood dial is turned.
//
// rigctld caches the frequency (500 ms by default), which made the display
// follow the dial in steps; on connecting, the cache is set to
// ifpan.rigctld_cache_ms (default 0: every poll reads the radio, ~32 ms per
// read at 57600 baud). If the radio does not answer, this is logged once
// and the "TS" readout turns grey until it answers again.
//
// Two-way (ifpan.cat_write, default on): when the receiver is tuned in
// piHPSDR-IF (click, wheel, keypad, ...), the new frequency is sent to the
// Kenwood ("F"), which then becomes the new display centre. Only the latest
// request is sent (no queue), CAT-driven changes are not echoed back, and
// for a short while after a set, stale readings of the old frequency are
// ignored so that the display does not jump back.
//

#include <gtk/gtk.h>
#include <wdsp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "audio.h"
#include "client_server.h"
#include "ext.h"
#include "filter.h"
#include "ifpan.h"
#include "main.h"
#include "message.h"
#include "mode.h"
#include "property.h"
#include "radio.h"
#ifdef RADE
#include "rade_glue.h"
#endif
#include "receiver.h"
#include "sliders.h"
#include "soapy_protocol.h"
#include "vfo.h"

long long ifpan_if_freq = IFPAN_DEFAULT_IF_FREQ;
long long ifpan_lo_offset = 50000;
char      ifpan_rigctld_host[64] = "127.0.0.1";
int       ifpan_rigctld_port = 4532;
int       ifpan_poll_ms = 50;
int       ifpan_cat_connected = 0;
int       ifpan_cat_write = 1;
int       ifpan_follow_vfo = 0;
int       ifpan_rigctld_cache_ms = 0;
int       ifpan_kenwood_mode = -1;
int       ifpan_kenwood_smeter = -999;
#define CAT_SMETER_EVERY 3            // read the S-meter every 3 polls

//
// Kenwood transmitting (Kenwood TCP only, from the "IF;" status): the RX
// audio can be muted (ifpan.tx_mute, Radio menu), and the meter shows the
// TS-570D TX meters instead of its S-meter. Raw meter values as the radio
// reports them (-1: not read yet); meter.c turns them into W / SWR.
//
int       ifpan_tx_mute = 1;
int       ifpan_follow_mode = 1;      // receiver mode follows the Kenwood mode
int       ifpan_sdr_lna_off = 0;      // SDR front-end LNA kept off (strong IF input; Radio menu)
volatile long ifpan_diag_heartbeats = 0;   // TX chain heartbeats from the SDR (diagnostics)
int       ifpan_kenwood_tx = 0;
int       ifpan_kenwood_pwr = -1;     // "SM;" while transmitting
int       ifpan_kenwood_swr = -1;     // "RM1nnnn;"
int       ifpan_kenwood_alc = -1;     // "RM3nnnn;"

//
// PTT from piHPSDR-IF (toolbar PTT / MOX): the CAT thread sends "TX;" /
// "RX;" (rigctld: "T 1" / "T 0"); the microphone stays on the radio. Only
// the latest request is kept. A TX request is dropped while CAT is down,
// an RX request is kept until it has been sent.
//
static int ptt_request = -1;          // -1: none, 0: RX, 1: TX (cat_mutex)
static volatile int ptt_keyed = 0;    // 1: piHPSDR-IF has put the Kenwood in TX

//
// CAT link: either Hamlib rigctld (which owns the serial port), or the
// radio's own Kenwood protocol over TCP straight to an RS-232/WiFi bridge
// (ESP32/ts570_wifi_bridge, or ser2net/socat), without rigctld.
//
int       ifpan_cat_backend = IFPAN_CAT_RIGCTLD;
char      ifpan_kenwood_host[64] = "ts570.local";
int       ifpan_kenwood_port = 7373;
static volatile int cat_reconnect = 0;   // set by the menu: close and reconnect

//
// Kenwood mode as shown next to "TS" (short names), and the Hamlib names
// rigctld reports for them (same order).
//
const char *ifpan_mode_names[] = { "USB", "LSB", "CW", "CW-R", "AM", "FM", "FSK", "FSKR", NULL };
static const char *hamlib_mode_names[] = { "USB", "LSB", "CW", "CWR", "AM", "FM", "RTTY", "RTTYR", NULL };
#define CAT_MODE_EVERY 20             // read the mode every 20 polls (~1 s)

#define CAT_SET_HOLD_US 400000        // ignore stale readings this long after a set
#define TRUST_SET_US 400000           // no read-back until the knob has paused this long (was 150 ms: a
                                      // reading between wheel notches went unanswered, blocking 0.5 s)

static GThread *cat_thread_id = NULL;
static GMutex   cat_mutex;
static long long cat_pending = 0;     // last frequency read, waiting for the main loop
static int       cat_scheduled = 0;   // an idle callback is already queued
static long long cat_set_target = 0;  // frequency to send to the Kenwood (0: none)
static int       cat_set_mode = -1;   // mode to send to the Kenwood (ifpan_mode_names[] index, -1: none)
static int       preamp_request = -1; // toolbar PRE: 0 / 1 to send, -1: none
int              ifpan_kenwood_preamp = -1;   // TS preamp read by CAT: 0 / 1, -1 unknown

// toolbar PRE (main thread): toggle the Kenwood preamp
void ifpan_preamp_toggle(void) {
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_PREAMP, 0); return; }
  if (!ifpan_cat_connected) { return; }
  g_mutex_lock(&cat_mutex);
  preamp_request = (ifpan_kenwood_preamp == 1) ? 0 : 1;
  g_mutex_unlock(&cat_mutex);
}

//
// Toolbar "TS MUTE": the Kenwood's speaker (AF gain) to 0 and back to the
// level it had, to compare its audio with piHPSDR-IF's without touching
// the VOL knob. Turning VOL while muted ends the mute; the level is put
// back on exit.
//
static int       ts_mute_request = -1;   // 1 mute, 0 unmute, -1 none (cat_mutex)
int              ifpan_ts_muted = 0;     // 1: the Kenwood's AF gain is held at 0
static int       ts_af_saved = 0;        // AF gain before muting (CAT thread)

void ifpan_ts_mute_toggle(void) {
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_TS_MUTE, 0); return; }
  if (!ifpan_cat_connected) { return; }
  g_mutex_lock(&cat_mutex);
  ts_mute_request = ifpan_ts_muted ? 0 : 1;
  g_mutex_unlock(&cat_mutex);
}

static int       att_request = -1;    // toolbar ATT: 0 / 1 to send, -1: none
int              ifpan_kenwood_att = -1;      // TS attenuator read by CAT: 0 / 1, -1 unknown

// toolbar ATT (main thread): toggle the Kenwood attenuator
void ifpan_att_toggle(void) {
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_ATT, 0); return; }
  if (!ifpan_cat_connected) { return; }
  g_mutex_lock(&cat_mutex);
  att_request = (ifpan_kenwood_att == 1) ? 0 : 1;
  g_mutex_unlock(&cat_mutex);
}

//
// TS-570D automatic antenna tuner (Kenwood TCP only). "AC;" answers
// "AC" + RX-AT + TX-AT + tuning, e.g. "AC010" = tuner in line, not tuning
// (checked on the radio). It is switched with "AC 10;" / "AC 00;" (Hamlib's
// TS-570 format, checked: the tuner relay switches) and a tuning cycle is
// started with "AC 11;" (the radio transmits a carrier while tuning).
//
#define ATU_OFF        0
#define ATU_ON         1
#define ATU_TUNE       2
#define ATU_TUNE_STOP  3
static int       atu_request = -1;    // ATU_* to send, -1: none
int              ifpan_kenwood_atu = -1;      // tuner in line: 0 / 1, -1 unknown
int              ifpan_kenwood_atu_tuning = 0; // 1 while the tuner is tuning

// toolbar "ATU" (main thread): tuner in line / through
void ifpan_atu_toggle(void) {
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_ATU, 0); return; }
  if (!ifpan_cat_connected) { return; }
  g_mutex_lock(&cat_mutex);
  atu_request = (ifpan_kenwood_atu == 1) ? ATU_OFF : ATU_ON;
  g_mutex_unlock(&cat_mutex);
}

// toolbar "ATU tune" (main thread): start a tuning cycle, or stop it
void ifpan_atu_tune(void) {
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_ATU_TUNE, 0); return; }
  if (!ifpan_cat_connected) { return; }
  g_mutex_lock(&cat_mutex);
  atu_request = ifpan_kenwood_atu_tuning ? ATU_TUNE_STOP : ATU_TUNE;
  g_mutex_unlock(&cat_mutex);
}

//
// TS-570D output power: the TX Drive slider is the radio's PWR setting.
// "PCnnn;" sets it, "PC;" reads it (checked on the radio: 5 W steps, 5..100,
// AM 5..25, kept per mode; 10 / 20 W set gave SM 2 / 4 in TX). Turning PWR
// on the radio moves the slider. Readings are ignored for a while after
// the slider was moved, so that a reading taken before the new value was
// sent does not pull the slider back.
//
#define PC_HOLD_US 1500000
static int       pc_request = -1;     // watts to send, -1: none
int              ifpan_kenwood_pc = -1;       // PWR setting read by CAT (W), -1 unknown
static gint64    pc_touched = 0;      // when the slider was last moved (main thread)

//
// TUNE: piHPSDR's TUNE tone (a 1 kHz tone in IFPAN, see tx_set_signal) is
// sent to ACC2 and the Kenwood keyed as for MOX. The CAT thread first sets
// the tune power ("Tune Drive level" in W, or the TX Drive with "Tune use
// drive"), remembering the radio's PWR setting to put it back after. In CW
// and FSK the Kenwood ignores ACC2 audio, so it is switched to USB (CW-R /
// FSK-R: LSB) for the tune and back afterwards; meanwhile its mode readings
// are not applied to the receiver.
//
static int       tune_request = -1;   // 1 start, 0 stop, -1 none (cat_mutex)
static int       tune_watts = 10;     // tune power (cat_mutex)
static int       tune_pc_dirty = 0;   // tune power changed while tuning (cat_mutex)
static int       tune_active = 0;     // CAT thread only
static int       tune_stopping = 0;   // CAT thread only: restore once in RX
static int       tune_saved_mode = -1;
static int       tune_saved_pc = -1;

static int       tune_is_twotone = 0; // the signal is TwoTone: TX Drive power (cat_mutex)
static int       tune_drive_watts = 100; // TX Drive in W, set again at the end (cat_mutex)

static int clamp_watts(double w) {
  int i = (int)(w + 0.5);
  return (i < 5) ? 5 : (i > 100) ? 100 : i;
}

// tune power in W from the TX menu settings (main thread)
static int tune_power(void) {
  if (transmitter == NULL) { return 10; }
  if (tune_is_twotone || transmitter->tune_use_drive) { return clamp_watts(transmitter->drive); }
  return clamp_watts(transmitter->tune_drive);
}

//
// TUNE / TwoTone on / off on the server (radio_set_tune, radio_set_twotone,
// radio_set_mox), main thread. TwoTone is handled like TUNE (same ACC2 mode
// switch in CW / FSK) but at the TX Drive power.
//
void ifpan_tune(int on, int twotone) {
  if (radio_is_remote) { return; }
  g_mutex_lock(&cat_mutex);
  if (on || tune_request == 1 || tune_active) { tune_request = on; }
  if (on) { tune_is_twotone = twotone; }
  tune_watts = tune_power();
  tune_drive_watts = clamp_watts(transmitter != NULL ? transmitter->drive : 100.0);
  g_mutex_unlock(&cat_mutex);
}

// "Tune Drive level" / "Tune use drive" / Drive changed: while tuning, the
// new tune power is sent at once
void ifpan_tune_power_changed(void) {
  if (radio_is_remote || transmitter == NULL || !(transmitter->tune || transmitter->twotone)) { return; }
  g_mutex_lock(&cat_mutex);
  tune_watts = tune_power();
  tune_drive_watts = clamp_watts(transmitter->drive);
  tune_pc_dirty = 1;
  g_mutex_unlock(&cat_mutex);
}

// TX Drive slider moved (main thread, client and server)
void ifpan_set_power(double drive) {
  pc_touched = g_get_monotonic_time();
  if (radio_is_remote || !ifpan_cat_connected) { return; }
  if (transmitter != NULL && (transmitter->tune || transmitter->twotone)) {
    ifpan_tune_power_changed();       // the TX Drive power is set again after the tune
    return;
  }
  int w = (int)(drive + 0.5);
  g_mutex_lock(&cat_mutex);
  pc_request = (w < 5) ? 5 : (w > 100) ? 100 : w;
  g_mutex_unlock(&cat_mutex);
}

// the radio's PWR setting changed (main thread): move the slider
int ifpan_power_apply(gpointer data) {
  int w = GPOINTER_TO_INT(data);
  if (transmitter == NULL || w < 0) { return G_SOURCE_REMOVE; }
  if (g_get_monotonic_time() - pc_touched < PC_HOLD_US) { return G_SOURCE_REMOVE; }
  if (transmitter->drive < 0.5) { return G_SOURCE_REMOVE; }  // zero drive (SWR protection) stays
  if (transmitter->tune || transmitter->twotone) { return G_SOURCE_REMOVE; }  // tune power
#ifdef RADE
  if (!radio_is_remote && rade_glue_get_tx_armed() && w > RADE_DRIVE_MAX + 0.5) {
    radio_set_drive(RADE_DRIVE_MAX);   // PWR turned up on the TS itself: back to the RADE limit
    return G_SOURCE_REMOVE;
  }
#endif
  if ((int)(transmitter->drive + 0.5) != w) {
    transmitter->drive = w;
    if (!radio_is_remote) { radio_calc_drive_level(0); }
    g_idle_add(sliders_drive, GINT_TO_POINTER(100 * suppress_popup_sliders));
  }
  return G_SOURCE_REMOVE;
}

static long long ui_last_freq = -1;   // receiver frequency last seen (main thread only)
static gint64    ui_tuned_us = 0;     // when the receiver was last tuned here (main thread only)
static int       ui_last_mode = -1;   // receiver mode last seen (main thread only)

//
// Frequency the PlutoSDR LO is tuned to: the IF centre plus the LO offset
// (with the radio's frequency calibration applied). The offset moves the
// Pluto's DC spike away from the Kenwood VFO; soapy_protocol.c shifts the
// samples back digitally so the IF centre stays in the middle of the display.
//
//
// IF offset per Kenwood mode (Hz, index = ifpan_mode_names[]): the TS-570D
// moves its 1st LO with the mode so that each mode's pass band sits in its
// IF filters, so the carrier is not at the same point of the 73.05 MHz IF
// in USB, LSB, CW, AM, ... (seen 2026-10-11: a station at 7.153 LSB on the
// TS was at 7.156 in piHPSDR-IF, while USB was right). The value of the
// Kenwood's current mode is added to the IF; set in the Radio menu.
//
int ifpan_mode_if_offset[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

long long ifpan_hw_frequency(void) {
  long long off = (ifpan_kenwood_mode >= 0 && ifpan_kenwood_mode < 8) ? ifpan_mode_if_offset[ifpan_kenwood_mode] : 0;
  return calibrated_frequency(ifpan_if_freq + off + ifpan_lo_offset);
}

// retune the SDR (main thread): after a Kenwood mode change or a new offset
static gboolean ifpan_retune_sdr(gpointer data) {
  if (radio_is_remote) { return G_SOURCE_REMOVE; }
  for (int id = 0; id < receivers; id++) { soapy_protocol_set_rx_frequency(id); }
  return G_SOURCE_REMOVE;
}

// Radio menu: IF offset (Hz) of the Kenwood's current mode, applied at once
void ifpan_set_mode_if_offset(int hz) {
  int m = ifpan_kenwood_mode;
  if (m < 0 || m > 7 || radio_is_remote) { return; }
  if (hz > 10000) { hz = 10000; }
  if (hz < -10000) { hz = -10000; }
  ifpan_mode_if_offset[m] = hz;
  ifpan_retune_sdr(NULL);
}

void ifpan_save_state(void) {
  SetPropI0("ifpan.if_freq",       ifpan_if_freq);
  SetPropI0("ifpan.lo_offset",     ifpan_lo_offset);
  SetPropS0("ifpan.rigctld_host",  ifpan_rigctld_host);
  SetPropI0("ifpan.rigctld_port",  ifpan_rigctld_port);
  SetPropI0("ifpan.poll_ms",       ifpan_poll_ms);
  SetPropI0("ifpan.cat_write",     ifpan_cat_write);
  SetPropI0("ifpan.follow_vfo",    ifpan_follow_vfo);
  SetPropI0("ifpan.rigctld_cache_ms", ifpan_rigctld_cache_ms);
  SetPropI0("ifpan.cat_backend",   ifpan_cat_backend);
  SetPropS0("ifpan.kenwood_host",  ifpan_kenwood_host);
  SetPropI0("ifpan.kenwood_port",  ifpan_kenwood_port);
  SetPropI0("ifpan.tx_mute",       ifpan_tx_mute);
  SetPropI0("ifpan.follow_mode",   ifpan_follow_mode);
  SetPropI0("ifpan.sdr_lna_off",   ifpan_sdr_lna_off);
  SetPropS0("ifpan.tx_out",        ifpan_tx_out);
  for (int m = 0; m < 8; m++) { SetPropI1("ifpan.mode_if_offset.%d", m, ifpan_mode_if_offset[m]); }
}

void ifpan_restore_state(void) {
  GetPropI0("ifpan.if_freq",       ifpan_if_freq);
  GetPropS0("ifpan.rigctld_host",  ifpan_rigctld_host);
  GetPropI0("ifpan.rigctld_port",  ifpan_rigctld_port);
  GetPropI0("ifpan.poll_ms",       ifpan_poll_ms);
  GetPropI0("ifpan.lo_offset",     ifpan_lo_offset);
  GetPropI0("ifpan.cat_write",     ifpan_cat_write);
  GetPropI0("ifpan.follow_vfo",    ifpan_follow_vfo);
  GetPropI0("ifpan.rigctld_cache_ms", ifpan_rigctld_cache_ms);
  GetPropI0("ifpan.cat_backend",   ifpan_cat_backend);
  GetPropS0("ifpan.kenwood_host",  ifpan_kenwood_host);
  GetPropI0("ifpan.kenwood_port",  ifpan_kenwood_port);
  GetPropI0("ifpan.tx_mute",       ifpan_tx_mute);
  GetPropI0("ifpan.follow_mode",   ifpan_follow_mode);
  GetPropI0("ifpan.sdr_lna_off",   ifpan_sdr_lna_off);
  GetPropS0("ifpan.tx_out",        ifpan_tx_out);
  for (int m = 0; m < 8; m++) { GetPropI1("ifpan.mode_if_offset.%d", m, ifpan_mode_if_offset[m]); }
  if (ifpan_cat_backend != IFPAN_CAT_KENWOOD) { ifpan_cat_backend = IFPAN_CAT_RIGCTLD; }
  if (ifpan_poll_ms < 20) { ifpan_poll_ms = 20; }
  // keep the IF well inside the Pluto span (+/- 384 kHz at 768 kHz)
  if (ifpan_lo_offset >  250000) { ifpan_lo_offset =  250000; }
  if (ifpan_lo_offset < -250000) { ifpan_lo_offset = -250000; }
}

//
// While the Kenwood dial turns, the signals slide through the IF, and the
// display averaging (Display menu) smeared them: peaks got lower and
// distorted, and with fast tuning they almost vanished. When the display
// centre moves by df, the WDSP averaging history is moved by the same
// number of pixels (ShiftDisplayAverage), so the average keeps following
// the signals and looks the same while tuning as when standing still.
// Fractions of a pixel are carried over to the next move.
//
static void tuning_moved(long long df) {
  static double frac = 0.0;
  if (receivers < 1 || receiver[0] == NULL || receiver[0]->cB <= 0.0) { return; }
  double px = (double)df / receiver[0]->cB + frac;
  int n = (int)px;
  frac = px - n;
  if (n != 0) { ShiftDisplayAverage(receiver[0]->id, 0, n); }
}

//
// "Follow TS mode" (main thread): when the Kenwood mode changes, the
// piHPSDR-IF receiver switches to the same mode, so that the audio from
// the SDR matches the radio. Kenwood CW is the upper side (CW-R lower),
// FSK the lower side (FSK-R upper). Only changes of the Kenwood mode are
// applied, so a mode chosen in piHPSDR-IF stays until the Kenwood changes.
//
static int kenwood_mode_index(int mode);

static gboolean mode_apply(gpointer data) {
  // same order as ifpan_mode_names[]: USB LSB CW CW-R AM FM FSK FSKR
  static const int to_mode[8] = { modeUSB, modeLSB, modeCWU, modeCWL, modeAM, modeFMN, modeDIGL, modeDIGU };
  int m = GPOINTER_TO_INT(data);
  if (!ifpan_follow_mode || m < 0 || m > 7 || receivers < 1 || receiver[0] == NULL) { return G_SOURCE_REMOVE; }
  // a receiver mode the Kenwood shows as this one stays (DIGU in USB, DIGL in LSB)
  if (vfo[VFO_A].mode != to_mode[m] && kenwood_mode_index(vfo[VFO_A].mode) != m) {
    t_print("%s: Kenwood %s -> receiver %s\n", __func__, ifpan_mode_names[m], mode_string[to_mode[m]]);
    vfo_id_mode_changed(VFO_A, to_mode[m]);
    ui_last_mode = to_mode[m];        // came from the Kenwood: not to be sent back
    ifpan_push_vfo_to_client();
  }
  return G_SOURCE_REMOVE;
}

//
// Server: piHPSDR only tells a client about mode / filter changes the
// client asked for itself. After changes made here (the Kenwood's mode,
// RADE switching to DIGU / DIGL, Fit Filter, ...), the same data is sent
// as after a client's mode change: VFOs, RX1, TX and the Var filters.
//
void ifpan_push_vfo_to_client(void) {
  if (radio_is_remote || !remoteclient.running) { return; }
  int m = vfo[VFO_A].mode;
  send_filter_var(remoteclient.sock_tcp, m, filterVar1);
  send_filter_var(remoteclient.sock_tcp, m, filterVar2);
  send_vfo_data(remoteclient.sock_tcp, VFO_A);
  send_vfo_data(remoteclient.sock_tcp, VFO_B);
  send_rx_data(remoteclient.sock_tcp, 0);
  if (transmitter != NULL) { send_tx_data(remoteclient.sock_tcp); }
}

//
// piHPSDR mode -> index into ifpan_mode_names[] (the Kenwood's modes),
// -1 for modes the Kenwood does not have (DSB, SAM, SPEC, DRM). DIGU / DIGL
// are sound-card data modes (RADE, FT8, ...): their audio goes in through
// ACC2, so the Kenwood is in USB / LSB (its FSK modes are keyed by the FSK
// line and do not use ACC2 audio).
//
static int kenwood_mode_index(int mode) {
  switch (mode) {
  case modeUSB:  return 0;
  case modeLSB:  return 1;
  case modeCWU:  return 2;
  case modeCWL:  return 3;
  case modeAM:   return 4;
  case modeFMN:  return 5;
  case modeDIGL: return 1;
  case modeDIGU: return 0;
  default:       return -1;
  }
}

// Radio menu: switching "Follow TS mode" on applies the Kenwood mode at once
void ifpan_set_follow_mode(int on) {
  ifpan_follow_mode = on;
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_FOLLOW_MODE, on); return; }
  if (on && ifpan_kenwood_mode >= 0) { mode_apply(GINT_TO_POINTER(ifpan_kenwood_mode)); }
}

//
// While the receiver is being tuned here, the Kenwood follows a little
// behind, and the IF the SDR sees is centred on where the Kenwood really is.
// Move only the display centre there (and the WDSP display average with it),
// not the receiver (CTUN frequency): the receiver stays on the target being
// tuned, its offset from the centre is recomputed so the audio stays right,
// and the sliding display (ifpan_display_shift) draws the IF data where it
// belongs. Without this, a continuously turning knob (MIDI VFO) froze the
// centre until it stopped, and the display / audio were off by the distance
// the Kenwood had already travelled.
//
//
// Tuning diagnostics: if a file "ifpan-tunelog" exists in the settings folder
// at start, every tuning step made here, every FA sent to the Kenwood, every
// Kenwood reading and every display re-centre is logged with milliseconds
// ("tune:" lines), to see where a slide stops being fluid.
//
static int tunelog_on = 0;
#define TUNELOG(...) do { if (tunelog_on) { t_print("tune: " __VA_ARGS__); } } while (0)

static void centre_only(long long f) {
  if (f <= 0 || receivers < 1 || receiver[0] == NULL || !vfo[VFO_A].ctun) { return; }
  if (vfo[VFO_A].frequency == f) { return; }
  TUNELOG("centre %lld -> %lld (receiver %lld)\n", vfo[VFO_A].frequency, f, vfo[VFO_A].ctun_frequency);
  tuning_moved(f - vfo[VFO_A].frequency);
  vfo[VFO_A].frequency = f;
  rx_frequency_changed(receiver[0]);
  g_idle_add(ext_vfo_update, NULL);
}

static long long centre_pending = 0;  // Kenwood reading taken while a set is on its way
static int       centre_scheduled = 0;

static gboolean centre_apply(gpointer data) {
  long long f;
  g_mutex_lock(&cat_mutex);
  f = centre_pending;
  centre_scheduled = 0;
  g_mutex_unlock(&cat_mutex);
  centre_only(f);
  return G_SOURCE_REMOVE;
}

static gboolean cat_apply(gpointer data) {
  long long f;
  g_mutex_lock(&cat_mutex);
  f = cat_pending;
  cat_scheduled = 0;
  g_mutex_unlock(&cat_mutex);
  if (ifpan_cat_write) {
    //
    // While the receiver is being tuned here (mouse wheel), a Kenwood
    // reading taken before the newest target reached it made the display
    // flick back a step: do not move the receiver while a change is not yet
    // sent (ui_watch has not seen it) and for 0.5 s after the last one;
    // only the display centre follows the Kenwood meanwhile
    //
    long long cur = vfo[VFO_A].ctun ? vfo[VFO_A].ctun_frequency : vfo[VFO_A].frequency;
    if (cur != ui_last_freq || g_get_monotonic_time() - ui_tuned_us < 500000) {
      centre_only(f);
      return G_SOURCE_REMOVE;
    }
  }
  TUNELOG("apply %lld (centre was %lld, receiver %lld)\n", f, vfo[VFO_A].frequency, vfo[VFO_A].ctun_frequency);
  tuning_moved(f - vfo[VFO_A].frequency);
  vfo_id_set_center(VFO_A, f);
  // a re-centre caused by the Kenwood must not be sent back to it
  ui_last_freq = vfo[VFO_A].ctun ? vfo[VFO_A].ctun_frequency : vfo[VFO_A].frequency;
  return G_SOURCE_REMOVE;
}

//
// "Follow Kenwood": the receiver always sits on the Kenwood frequency, so
// turning the Kenwood dial tunes piHPSDR-IF too (instead of keeping the
// station being heard, which is the default CTUN behaviour). Switching it
// on moves the receiver to the Kenwood frequency at once (main thread).
//
void ifpan_set_follow(int on) {
  ifpan_follow_vfo = on;
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_FOLLOW_VFO, on); return; }
  if (on && receivers > 0 && receiver[0] != NULL && vfo[VFO_A].ctun) {
    vfo[VFO_A].ctun_frequency = vfo[VFO_A].frequency;
    rx_frequency_changed(receiver[0]);
    ui_last_freq = vfo[VFO_A].ctun_frequency;   // not to be echoed to the Kenwood
    g_idle_add(ext_vfo_update, NULL);
  }
}

//
// Display while tuning here with "Clicks tune the TS" (main thread, also
// in the client): the receiver moves at once, the IF contents only when
// the Kenwood has followed (CAT, a few hundred ms). Instead of letting the
// filter run ahead and jump back to the centre, the panadapter and the
// waterfall are drawn as if the Kenwood were already there: centre =
// receiver frequency, IF pixels moved by the same amount (an empty strip
// at one edge until the Kenwood catches up, then the shift is zero again).
// Only after a tuning step made here (the receiver frequency changed), for
// at most 2 s (no CAT answer: show the real position again). Turning the
// Kenwood dial with "Follow Kenwood" off keeps its CTUN offset unchanged.
//
#define IFPAN_SHIFT_HOLD_US 2000000

long long ifpan_display_shift(const RECEIVER *rx) {
  static long long last_ctun = -1;
  static gint64 moved_us = 0;
  if (rx == NULL || rx->id != 0 || !vfo[VFO_A].ctun) { return 0; }
  long long c = vfo[VFO_A].ctun_frequency;
  gint64 now = g_get_monotonic_time();
  if (c != last_ctun) {
    if (last_ctun >= 0) { moved_us = now; }
    last_ctun = c;
  }
  if (!ifpan_cat_write || now - moved_us > IFPAN_SHIFT_HOLD_US) { return 0; }
  return c - vfo[VFO_A].frequency;
}

const float *ifpan_shifted_pixels(const float *in, int width, long long shift_hz, double cAp, int slot) {
  static float *buf[2] = { NULL, NULL };
  static int len[2] = { 0, 0 };
  if (shift_hz == 0 || width <= 0 || slot < 0 || slot > 1) { return in; }
  if (len[slot] < width) {
    buf[slot] = g_renew(float, buf[slot], width);
    len[slot] = width;
  }
  int sp = (int)lround((double)shift_hz * cAp);
  for (int x = 0; x < width; x++) {
    int j = x + sp;
    buf[slot][x] = (j >= 0 && j < width) ? in[j] : -200.0F;
  }
  return buf[slot];
}

//
// Main loop, every 50 ms: if the receiver was tuned in piHPSDR-IF, ask the
// CAT thread to move the Kenwood there (only the latest target is kept).
//
static gboolean ui_watch(gpointer data) {
  if (receivers < 1 || receiver[0] == NULL) { return G_SOURCE_CONTINUE; }
  long long f = vfo[VFO_A].ctun ? vfo[VFO_A].ctun_frequency : vfo[VFO_A].frequency;
  if (ui_last_freq < 0) {
    ui_last_freq = f;
  } else if (f != ui_last_freq) {
    if (ifpan_cat_write && !ifpan_cat_connected) {
      // CAT link down for a moment: keep the step (ui_last_freq unchanged)
      // and send it once the Kenwood answers again, instead of losing it
      return G_SOURCE_CONTINUE;
    }
    ui_last_freq = f;
    if (ifpan_cat_write && ifpan_cat_connected) {
      TUNELOG("ui receiver %lld (centre %lld)\n", f, vfo[VFO_A].frequency);
      g_mutex_lock(&cat_mutex);
      cat_set_target = f;
      ui_tuned_us = g_get_monotonic_time();
      g_mutex_unlock(&cat_mutex);
    }
  }
  //
  // "Sync mode with TS": a mode chosen in piHPSDR-IF (mode menu, band
  // change, ...) is sent to the Kenwood as well (sent 300 ms later and
  // checked, see the CAT thread); changes coming from the Kenwood are not
  // sent back (mode_apply sets ui_last_mode).
  //
  int mode = vfo[VFO_A].mode;
  if (ui_last_mode < 0) {
    ui_last_mode = mode;
  } else if (mode != ui_last_mode) {
    ui_last_mode = mode;
    int k = kenwood_mode_index(mode);
    if (ifpan_follow_mode && ifpan_cat_connected && k >= 0 && k != ifpan_kenwood_mode) {
      t_print("%s: receiver mode %s -> Kenwood\n", __func__, mode_string[mode]);
      g_mutex_lock(&cat_mutex);
      cat_set_mode = k;
      g_mutex_unlock(&cat_mutex);
    }
  }
  return G_SOURCE_CONTINUE;
}

//
// Connect to the CAT server of the selected backend. rigctld gets a long
// receive timeout (its own retries when the radio does not answer); the
// Kenwood bridge a short one (the radio answers within ~40 ms or not at all).
//
static int cat_connect(void) {
  struct addrinfo hints, *res, *ai;
  char port[16];
  int sock = -1;
  int kenwood = (ifpan_cat_backend == IFPAN_CAT_KENWOOD);
  const char *host = kenwood ? ifpan_kenwood_host : ifpan_rigctld_host;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d", kenwood ? ifpan_kenwood_port : ifpan_rigctld_port);
  if (getaddrinfo(host, port, &hints, &res) != 0) { return -1; }
  for (ai = res; ai != NULL; ai = ai->ai_next) {
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) { continue; }
    if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) { break; }
    close(sock);
    sock = -1;
  }
  freeaddrinfo(res);
  if (sock >= 0) {
    struct timeval tv = { kenwood ? 0 : 6, kenwood ? 500000 : 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (kenwood) {
      int one = 1;
      setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
  }
  return sock;
}

void ifpan_cat_reconnect(void) {
  cat_reconnect = 1;
}

//
// PTT request from the toolbar (main thread): 1 TX, 0 RX, -1 toggle (MOX)
//
void ifpan_ptt(int on) {
  if (radio_is_remote) {
    // the server keys the Kenwood (and decides a MOX toggle from its own state)
    if (on < 0) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_MOX, 0); }
    else        { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_PTT, on); }
    return;
  }
  if (on < 0) { on = !(ifpan_kenwood_tx || ptt_keyed); }
  if (on && !ifpan_cat_connected) {
    t_print("%s: TX refused, no CAT link to the Kenwood\n", __func__);
    return;
  }
  g_mutex_lock(&cat_mutex);
  ptt_request = on;
  g_mutex_unlock(&cat_mutex);
}

//
// Toolbar "TS here": tune the Kenwood to the frequency piHPSDR-IF is
// receiving on (useful with "Clicks tune the TS" off, before transmitting).
// Sent like a click with two-way CAT; the display then re-centres on it.
//
void ifpan_ts_here(void) {
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_TS_HERE, 0); return; }
  if (!ifpan_cat_connected || receivers < 1 || receiver[0] == NULL) { return; }
  long long f = vfo[VFO_A].ctun ? vfo[VFO_A].ctun_frequency : vfo[VFO_A].frequency;
  g_mutex_lock(&cat_mutex);
  cat_set_target = f;
  g_mutex_unlock(&cat_mutex);
}

// ---------------------------------------------------------------------
// TX audio to the Kenwood
// ---------------------------------------------------------------------
//
// The WDSP TX chain (mic gain, EQ, leveler, CFC, compressor, TX filter,
// ALC; TX menu) processes the microphone - local sound card, or the remote
// client's, as in piHPSDR - and its 48 kHz output (soapy_protocol_iq_samples)
// is played on the sound card selected here, wired through an isolation
// transformer to the Kenwood's rear ACC2 audio input. A receiver structure
// of its own only carries this output stream, so the existing audio
// modules (PortAudio, PulseAudio, ALSA) are used unchanged.
//
char ifpan_tx_out[128] = "";          // sound card output to the Kenwood ("": none)
static RECEIVER txout_rx;

// (re)open the output (main thread); "" closes it
void ifpan_tx_out_select(const char *name) {
  char tmp[128];
  g_strlcpy(tmp, name ? name : "", sizeof(tmp));   // name may be ifpan_tx_out itself
  if (txout_rx.audio_handle != NULL) { audio_close_output(&txout_rx); }
  g_strlcpy(ifpan_tx_out, tmp, sizeof(ifpan_tx_out));
  if (ifpan_tx_out[0] == 0) { return; }
  txout_rx.id = 8;                    // only used in log lines / stream names
  g_strlcpy(txout_rx.audio_name, ifpan_tx_out, sizeof(txout_rx.audio_name));
  if (audio_open_output(&txout_rx) != 0) {
    t_print("%s: cannot open %s\n", __func__, ifpan_tx_out);
  } else {
    t_print("%s: TX audio to the Kenwood on %s\n", __func__, ifpan_tx_out);
  }
}

//
// TX audio diagnostics: if a file "ifpan-txdump" exists in the settings
// folder at start-up, the first 30 s of microphone input (as it enters the
// WDSP TX chain) and of processed output are written, while transmitting,
// as raw 32-bit float mono 48 kHz files txdump_mic.f32 / txdump_out.f32.
//
#define TXDUMP_MAX (48000 * 30)
static int    txdump_on = 0;
static FILE  *txdump_fp[2] = { NULL, NULL };
static long   txdump_n[2] = { 0, 0 };

//
// TX audio levels of every over (diagnostics, always on): the microphone
// reaching the TX chain (from the client on a server) and the processed
// audio written to the ACC2 sound card; logged when the Kenwood goes back
// to RX (txlev_report), to find overs without modulation.
//
static double txlev_peak[2], txlev_sum[2];
static long   txlev_n[2];

static void txlev_report(void) {
  double rms[2], pk[2];
  for (int i = 0; i < 2; i++) {
    rms[i] = txlev_n[i] > 0 ? 10.0 * log10(txlev_sum[i] / txlev_n[i] + 1e-20) : -200.0;
    pk[i] = 20.0 * log10(txlev_peak[i] + 1e-10);
  }
  t_print("tx audio: mic %.1f s rms %.1f peak %.1f dBFS | to ACC2 %.1f s rms %.1f peak %.1f dBFS%s\n",
          txlev_n[0] / 48000.0, rms[0], pk[0], txlev_n[1] / 48000.0, rms[1], pk[1],
          txout_rx.audio_handle == NULL ? " | ACC2 sound card NOT OPEN" : "");
  for (int i = 0; i < 2; i++) { txlev_peak[i] = 0.0; txlev_sum[i] = 0.0; txlev_n[i] = 0; }
}

void ifpan_tx_dump(int which, double s) {
  if (which >= 0 && which <= 1 && radio_is_transmitting()) {
    double a = fabs(s);
    if (a > txlev_peak[which]) { txlev_peak[which] = a; }
    txlev_sum[which] += s * s;
    txlev_n[which]++;
  }
  if (!txdump_on || !radio_is_transmitting() || which < 0 || which > 1) { return; }
  if (txdump_n[which] >= TXDUMP_MAX) { return; }
  if (txdump_fp[which] == NULL) {
    txdump_fp[which] = fopen(which ? "txdump_out.f32" : "txdump_mic.f32", "wb");
    if (txdump_fp[which] == NULL) { return; }
  }
  float f = (float) s;
  fwrite(&f, sizeof(f), 1, txdump_fp[which]);
  if (++txdump_n[which] == TXDUMP_MAX) {
    fclose(txdump_fp[which]);
    txdump_fp[which] = NULL;
    t_print("%s: %s written\n", __func__, which ? "txdump_out.f32" : "txdump_mic.f32");
  } else if ((txdump_n[which] % 48000) == 0) {
    fflush(txdump_fp[which]);
  }
}

// one 48 kHz sample of processed TX audio (TX thread)
void ifpan_tx_audio_sample(double s) {
  ifpan_tx_dump(1, s);
  if (txout_rx.audio_handle == NULL) { return; }
  audio_write(&txout_rx, s, s);
}

//
// Radio menu options without side effects: on a client they are passed
// to the server (which owns the Kenwood); the server sends the current
// values back with its periodic data.
//
void ifpan_set_cat_write(int on) {
  ifpan_cat_write = on;
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_CAT_WRITE, on); }
}

void ifpan_set_tx_mute(int on) {
  ifpan_tx_mute = on;
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_TX_MUTE, on); }
}

//
// Radio menu "SDR LO offset (kHz)": how far the SDR's LO sits from the IF.
// The SDR's DC spike (and on the Mirics MSi001 a weaker mirror on the other
// side) appears that far from the VFO; with an offset larger than half the
// displayed span they are out of sight. The SDR is retuned at once (the
// sample rotation follows by itself).
//
void ifpan_set_lo_offset_khz(int khz) {
  if (khz < 0) { khz = 0; }
  if (khz > 250) { khz = 250; }
  ifpan_lo_offset = 1000LL * khz;
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_LO_OFFSET, khz); return; }
  t_print("%s: SDR LO offset %d kHz\n", __func__, khz);
  for (int id = 0; id < receivers; id++) { soapy_protocol_set_rx_frequency(id); }
}

//
// Radio menu "SDR LNA off": the SDR's front-end LNA (Soapy gain element
// "LNA", e.g. the Mirics MSi001's +24 dB) is switched off after every RF
// gain setting (soapy_protocol_set_rx_gain). The IF buffer output is strong,
// and above about RF 80 the LNA made the MSi001 overload; the RF slider
// keeps working on the remaining stages. Applied at once.
//
void ifpan_set_sdr_lna_off(int on) {
  ifpan_sdr_lna_off = on ? 1 : 0;
  if (radio_is_remote) { send_ifpan(cl_sock_tcp, IFPAN_REMOTE_LNA_OFF, ifpan_sdr_lna_off); return; }
  t_print("%s: SDR LNA %s\n", __func__, ifpan_sdr_lna_off ? "kept off" : "as set by the RF gain");
  for (int id = 0; id < receivers; id++) { soapy_protocol_set_rx_gain(id); }
}

//
// Server side of client/server: a Kenwood action or IF panadapter option
// sent by the client (CMD_IFPAN), carried out here as if done locally
//
void ifpan_remote_command(int what, int value) {
  switch (what) {
  case IFPAN_REMOTE_PTT:         ifpan_ptt(value ? 1 : 0); break;
  case IFPAN_REMOTE_MOX:         ifpan_ptt(-1); break;
  case IFPAN_REMOTE_TS_HERE:     ifpan_ts_here(); break;
  case IFPAN_REMOTE_PREAMP:      ifpan_preamp_toggle(); break;
  case IFPAN_REMOTE_ATT:         ifpan_att_toggle(); break;
  case IFPAN_REMOTE_FOLLOW_VFO:  ifpan_set_follow(value); break;
  case IFPAN_REMOTE_CAT_WRITE:   ifpan_set_cat_write(value); break;
  case IFPAN_REMOTE_TX_MUTE:     ifpan_set_tx_mute(value); break;
  case IFPAN_REMOTE_FOLLOW_MODE: ifpan_set_follow_mode(value); break;
  case IFPAN_REMOTE_ATU:         ifpan_atu_toggle(); break;
  case IFPAN_REMOTE_ATU_TUNE:    ifpan_atu_tune(); break;
  case IFPAN_REMOTE_LO_OFFSET:   ifpan_set_lo_offset_khz(value); break;
  case IFPAN_REMOTE_LNA_OFF:     ifpan_set_sdr_lna_off(value); break;
  case IFPAN_REMOTE_TS_MUTE:     ifpan_ts_mute_toggle(); break;
  default:
    t_print("%s: unknown IF panadapter command %d\n", __func__, what);
    break;
  }
}

//
// Band / band-stack chosen in piHPSDR-IF (main thread): the Kenwood is sent
// to the new frequency and to the band stack's mode, also with "Clicks
// tune the TS" off (choosing a band is an explicit request). Modes the
// Kenwood does not have (DSB, SAM, DRM, ...) are not sent.
//
void ifpan_band_changed(long long f, int mode) {
  if (!ifpan_cat_connected) { return; }
  int m = kenwood_mode_index(mode);
  t_print("%s: Kenwood to %lld Hz, mode %s (receiver mode %s)\n", __func__, f,
          m >= 0 ? ifpan_mode_names[m] : "not sent", mode_string[mode]);
  g_mutex_lock(&cat_mutex);
  cat_set_target = f;
  if (m >= 0) { cat_set_mode = m; }
  g_mutex_unlock(&cat_mutex);
}

//
// On exit: if piHPSDR-IF keyed the Kenwood, put it back to receive, and
// give its speaker back its level if TS MUTE was on
// (wait up to 1 s for the CAT thread to send it)
//
void ifpan_stop(void) {
  if (ptt_keyed) {
    ifpan_ptt(0);
    for (int i = 0; i < 100 && ptt_keyed; i++) { usleep(10000); }
  }
  if (ifpan_ts_muted && ifpan_cat_connected) {
    // the Kenwood's speaker back to the level it had before TS MUTE
    g_mutex_lock(&cat_mutex);
    ts_mute_request = 0;
    g_mutex_unlock(&cat_mutex);
    for (int i = 0; i < 100 && ifpan_ts_muted; i++) { usleep(10000); }
  }
}

// ---------------------------------------------------------------------
// Kenwood CAT over TCP (TS-570D: commands and answers end with ';')
// ---------------------------------------------------------------------

//
// Read one answer up to ';'. Returns 0, -1 if the connection failed,
// -2 on timeout (the radio did not answer).
//
static int kw_read_frame(int sock, char *frame, size_t size) {
  size_t len = 0;
  for (;;) {
    char c;
    ssize_t n = recv(sock, &c, 1, 0);
    if (n == 0) { return -1; }
    if (n < 0) { return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1; }
    if (c == ';') {
      frame[len] = 0;
      return 0;
    }
    if (c == '\r' || c == '\n') { continue; }
    if (len < size - 1) { frame[len++] = c; }
  }
}

//
// Send a read command and wait for the answer starting with prefix.
// Other frames (auto-information, late answers) are skipped; "?" means
// the radio rejected the command. Returns 0, -1 (connection), -2 (no answer).
//
static int kw_query(int sock, const char *cmd, const char *prefix, char *frame, size_t size) {
  int len = strlen(cmd);
  size_t plen = strlen(prefix);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  for (int i = 0; i < 8; i++) {
    int rc = kw_read_frame(sock, frame, size);
    if (rc < 0) { return rc; }
    if (strcmp(frame, "?") == 0) { return -2; }
    if (strncmp(frame, prefix, plen) == 0) { return 0; }
  }
  return -2;
}

// "AI0;": no unsolicited frames, then drop whatever is still queued
static int kw_setup(int sock) {
  char frame[64];
  if (send(sock, "AI0;", 4, MSG_NOSIGNAL) != 4) { return -1; }
  while (kw_read_frame(sock, frame, sizeof(frame)) == 0) { }
  return 0;
}

//
// "IF;" -> "IF00014220000      000000 0002000008 ;": the VFO frequency
// (digits 2..12) and, at position 28, 1 while transmitting (same position
// as Hamlib's kenwood_get_ptt). One read gives both, so the TX state costs
// no extra command.
//
static long long kw_get_freq(int sock) {
  char frame[64];
  char digits[12];
  int rc = kw_query(sock, "IF;", "IF", frame, sizeof(frame));
  if (rc < 0) { return rc; }
  if (strlen(frame) < 30) { return -2; }
  memcpy(digits, frame + 2, 11);
  digits[11] = 0;
  char *end;
  long long f = strtoll(digits, &end, 10);
  if (end == digits || f <= 0) { return -2; }
  int tx = (frame[28] == '1');
  if (tx != ifpan_kenwood_tx) {
    ifpan_kenwood_tx = tx;
    g_idle_add(ext_vfo_update, NULL);
  }
  return f;
}

// "FA00014074000;" (no answer)
static int kw_set_freq(int sock, long long f) {
  char cmd[32];
  int len = snprintf(cmd, sizeof(cmd), "FA%011lld;", f);
  return (send(sock, cmd, len, MSG_NOSIGNAL) == len) ? 0 : -1;
}

// "MD;" -> "MDn;"  (1 LSB, 2 USB, 3 CW, 4 FM, 5 AM, 6 FSK, 7 CW-R, 9 FSK-R)
static int kw_get_mode(int sock) {
  // index into ifpan_mode_names[] for MD 0..9 (-3: not one of them)
  static const int md_index[10] = { -3, 1, 0, 2, 5, 4, 6, 3, -3, 7 };
  char frame[64];
  int rc = kw_query(sock, "MD;", "MD", frame, sizeof(frame));
  if (rc < 0) { return rc; }
  int md = frame[2] - '0';
  return (md >= 0 && md <= 9) ? md_index[md] : -3;
}

//
// "SM;" -> S-meter 0000..0015 (last 4 digits of the answer). Shown like the
// radio's own scale: 1..9 = S1..S9 (6 dB per S unit), 10..15 = S9+10..+60.
//
static int kw_get_strength(int sock, int *db) {
  char frame[64];
  int rc = kw_query(sock, "SM;", "SM", frame, sizeof(frame));
  if (rc < 0) { return rc; }
  size_t len = strlen(frame);
  if (len < 6) { return -2; }
  int raw = atoi(frame + len - 4);
  *db = (raw <= 9) ? (raw - 9) * 6 : (raw - 9) * 10;
  return 0;
}

//
// TX meters, once per poll while the Kenwood transmits:
//  - "SM;" reads the power meter in TX;
//  - "RM;" reads only the meter selected on the radio ("RMpnnnn;", p = 1 SWR,
//    2 COMP, 3 ALC); "RM1;" / "RM3;" select one. SWR and ALC are read in
//    turn: each poll reads the meter selected on the previous poll, then
//    selects the other one. The meter the user had selected is put back
//    when the radio returns to receive.
// Returns 0, or -1 if the connection failed.
//
static int kw_meter_user = 0;         // RM selection before TX (0: not in TX)
static int kw_high_swr = 0;           // consecutive SWR readings above the alarm
static gint64 kw_high_since = 0;      // since when (monotonic us)

//
// SWR protection (TX menu "SWR alarm at" / "SWR Protection"), as piHPSDR
// does with the HPSDR's own SWR: the drive slider goes to zero, so VOX or
// PTT do not put audio into a bad antenna again until the operator acts.
//
static gboolean swr_protect_cb(gpointer data) {
  if (transmitter != NULL && transmitter->tune) { radio_set_tune(0); }
  if (mox) { radio_set_mox(0); }
  radio_set_drive(0.0);
  return G_SOURCE_REMOVE;
}

static int kw_tx_meters(int sock) {
  char frame[64];
  char cmd[8];
  if (!ifpan_kenwood_tx) {
    kw_high_swr = 0;
    if (kw_meter_user != 0) {
      snprintf(cmd, sizeof(cmd), "RM%d;", kw_meter_user);
      kw_meter_user = 0;
      ifpan_kenwood_pwr = ifpan_kenwood_swr = ifpan_kenwood_alc = -1;
      if (send(sock, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t) strlen(cmd)) { return -1; }
    }
    return 0;
  }
  int rc = kw_query(sock, "SM;", "SM", frame, sizeof(frame));
  if (rc == -1) { return -1; }
  if (rc == 0 && strlen(frame) >= 6) { ifpan_kenwood_pwr = atoi(frame + strlen(frame) - 4); }
  rc = kw_query(sock, "RM;", "RM", frame, sizeof(frame));
  if (rc == -1) { return -1; }
  if (rc == 0 && strlen(frame) >= 7) {
    int sel = frame[2] - '0';
    int val = atoi(frame + 3);
    if (kw_meter_user == 0) { kw_meter_user = (sel >= 1 && sel <= 3) ? sel : 2; }
    if (sel == 1) {
      ifpan_kenwood_swr = val;
      //
      // Two readings in a row above the alarm (not an artifact), and not
      // while the tuner tunes: warn, and with protection on drop the
      // Kenwood to RX at once, then MOX / TUNE off and drive to zero.
      // During TUNE (e.g. adjusting an external tuner) the SWR may stay
      // high for up to 10 s before the protection acts.
      //
      double swr = (val >= 24) ? 9.9 : 60.0 / (30.0 - val) - 1.0;
      if (transmitter != NULL && swr >= transmitter->swr_alarm && !ifpan_kenwood_atu_tuning) {
        gint64 now = g_get_monotonic_time();
        if (kw_high_swr++ == 0) { kw_high_since = now; }
        if (kw_high_swr >= 2) { high_swr_seen = 1; }
        int trip = transmitter->tune ? (now - kw_high_since >= 10000000) : (kw_high_swr >= 2);
        if (trip && transmitter->swr_protection) {
          t_print("%s: SWR %.1f >= %.1f, TX stopped\n", __func__, swr, transmitter->swr_alarm);
          if (send(sock, "RX;", 3, MSG_NOSIGNAL) != 3) { return -1; }
          ptt_keyed = 0;
          kw_high_swr = 0;
          g_idle_add(swr_protect_cb, NULL);
        }
      } else {
        kw_high_swr = 0;
      }
    }
    if (sel == 3) { ifpan_kenwood_alc = val; }
    snprintf(cmd, sizeof(cmd), "RM%d;", sel == 1 ? 3 : 1);
    if (send(sock, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t) strlen(cmd)) { return -1; }
  }
  return 0;
}

//
// Read one line from rigctld. Returns 0, or -1 if the connection failed.
//
static int cat_read_line(int sock, char *line, size_t size) {
  size_t len = 0;
  while (len < size - 1) {
    ssize_t n = recv(sock, &line[len], 1, 0);
    if (n <= 0) { return -1; }
    if (line[len] == '\n') { break; }
    len++;
  }
  line[len] = 0;
  return 0;
}

//
// Send "f\n" and read one line. Returns the frequency, -1 if the connection
// to rigctld failed, or -2 if rigctld answered but the radio did not.
//
static long long cat_get_freq(int sock) {
  char line[64];
  if (send(sock, "f\n", 2, MSG_NOSIGNAL) != 2) { return -1; }
  if (cat_read_line(sock, line, sizeof(line)) < 0) { return -1; }
  if (strncmp(line, "RPRT", 4) == 0) { return -2; }   // rigctld error report
  char *end;
  long long f = strtoll(line, &end, 10);
  if (end == line || f <= 0) { return -2; }
  return f;
}

//
// Send "m\n" (reply: mode line + passband line). Returns the index into
// ifpan_mode_names[] (-3 if the mode is not one of them), -1 if the
// connection failed, -2 if the radio did not answer.
//
static int cat_get_mode(int sock) {
  char line[64], pb[64];
  if (send(sock, "m\n", 2, MSG_NOSIGNAL) != 2) { return -1; }
  if (cat_read_line(sock, line, sizeof(line)) < 0) { return -1; }
  if (strncmp(line, "RPRT", 4) == 0) { return -2; }
  if (cat_read_line(sock, pb, sizeof(pb)) < 0) { return -1; }   // passband, not used
  for (int i = 0; hamlib_mode_names[i] != NULL; i++) {
    if (strcmp(line, hamlib_mode_names[i]) == 0) { return i; }
  }
  return -3;
}

//
// Send "l STRENGTH\n". Returns 0 and the S-meter in dB relative to S9 in
// *db, -1 if the connection failed, -2 if the radio did not answer.
//
static int cat_get_strength(int sock, int *db) {
  char line[64];
  const char *cmd = "l STRENGTH\n";
  int len = strlen(cmd);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  if (cat_read_line(sock, line, sizeof(line)) < 0) { return -1; }
  if (strncmp(line, "RPRT", 4) == 0) { return -2; }
  char *end;
  long v = strtol(line, &end, 10);
  if (end == line) { return -2; }
  *db = (int) v;
  return 0;
}

//
// Set the rigctld frequency cache timeout. Returns -1 if the connection failed.
//
static int cat_set_cache(int sock, int ms) {
  char cmd[32], line[64];
  int len = snprintf(cmd, sizeof(cmd), "\\set_cache %d\n", ms);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  if (cat_read_line(sock, line, sizeof(line)) < 0) { return -1; }
  t_print("%s: rigctld cache %d ms: %s\n", __func__, ms, line);
  return 0;
}

//
// Send "F <freq>\n" and read the "RPRT n" reply. Returns -1 if the
// connection failed, otherwise 0 (a refusal by the radio is only logged).
//
static int cat_set_freq(int sock, long long f) {
  char cmd[32], line[64];
  int len = snprintf(cmd, sizeof(cmd), "F %lld\n", f);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  if (cat_read_line(sock, line, sizeof(line)) < 0) { return -1; }
  if (strncmp(line, "RPRT 0", 6) != 0) {
    t_print("%s: rigctld refused F %lld: %s\n", __func__, f, line);
  }
  return 0;
}

// ---------------------------------------------------------------------
// Backend dispatch (same return conventions for both backends)
// ---------------------------------------------------------------------

static const char *cat_name(void) {
  return (ifpan_cat_backend == IFPAN_CAT_KENWOOD) ? "Kenwood TCP" : "rigctld";
}

static int be_setup(int sock) {
  return (ifpan_cat_backend == IFPAN_CAT_KENWOOD) ? kw_setup(sock) : cat_set_cache(sock, ifpan_rigctld_cache_ms);
}

static long long be_get_freq(int sock) {
  return (ifpan_cat_backend == IFPAN_CAT_KENWOOD) ? kw_get_freq(sock) : cat_get_freq(sock);
}

static int be_set_freq(int sock, long long f) {
  return (ifpan_cat_backend == IFPAN_CAT_KENWOOD) ? kw_set_freq(sock, f) : cat_set_freq(sock, f);
}

static int be_get_mode(int sock) {
  return (ifpan_cat_backend == IFPAN_CAT_KENWOOD) ? kw_get_mode(sock) : cat_get_mode(sock);
}

//
// TS-570 preamp (12 dB): Kenwood "PA;" -> "PA0;" / "PA1;", set "PA0;" / "PA1;";
// rigctld "l PREAMP" -> 0 / 12, set "L PREAMP 0|12". get returns 0/1, -1 if
// the connection failed, -2 if there was no usable answer.
//
static int be_get_preamp(int sock) {
  char frame[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int rc = kw_query(sock, "PA;", "PA", frame, sizeof(frame));
    if (rc < 0) { return rc; }
    return (frame[2] == '1') ? 1 : (frame[2] == '0') ? 0 : -2;
  }
  const char *cmd = "l PREAMP\n";
  if (send(sock, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t) strlen(cmd)) { return -1; }
  if (cat_read_line(sock, frame, sizeof(frame)) < 0) { return -1; }
  if (strncmp(frame, "RPRT", 4) == 0) { return -2; }
  return atoi(frame) > 0;
}

//
// TS-570D antenna tuner (see ifpan_atu_toggle): Kenwood TCP only. get
// returns in line (bit 0) + tuning (bit 1), -1 if the connection failed,
// -2 if there was no usable answer (always with rigctld).
//
static int be_get_atu(int sock) {
  char frame[64];
  if (ifpan_cat_backend != IFPAN_CAT_KENWOOD) { return -2; }
  int rc = kw_query(sock, "AC;", "AC", frame, sizeof(frame));
  if (rc < 0) { return rc; }
  if (strlen(frame) < 5) { return -2; }
  return (frame[3] == '1' ? 1 : 0) | (frame[4] == '1' ? 2 : 0);
}

static int be_set_atu(int sock, int req) {
  if (ifpan_cat_backend != IFPAN_CAT_KENWOOD) { return 0; }
  const char *cmd = (req == ATU_OFF) ? "AC 00;" : (req == ATU_TUNE) ? "AC 11;" : "AC 10;";
  t_print("%s: %s\n", __func__, cmd);
  return (send(sock, cmd, strlen(cmd), MSG_NOSIGNAL) == (ssize_t) strlen(cmd)) ? 0 : -1;
}

//
// TS-570 attenuator (18 dB): Kenwood "RA;" -> "RA00;" / "RA01;", set
// "RA00;" / "RA01;"; rigctld "l ATT" -> 0 / 18, set "L ATT 0|18". Same
// return values as the preamp functions.
//
static int be_get_att(int sock) {
  char frame[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int rc = kw_query(sock, "RA;", "RA", frame, sizeof(frame));
    if (rc < 0) { return rc; }
    if (strlen(frame) < 4) { return -2; }
    return atoi(frame + 2) > 0;
  }
  const char *cmd = "l ATT\n";
  if (send(sock, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t) strlen(cmd)) { return -1; }
  if (cat_read_line(sock, frame, sizeof(frame)) < 0) { return -1; }
  if (strncmp(frame, "RPRT", 4) == 0) { return -2; }
  return atoi(frame) > 0;
}

static int be_set_att(int sock, int on) {
  char cmd[32], line[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int len = snprintf(cmd, sizeof(cmd), "RA%02d;", on ? 1 : 0);
    return (send(sock, cmd, len, MSG_NOSIGNAL) == len) ? 0 : -1;
  }
  int len = snprintf(cmd, sizeof(cmd), "L ATT %d\n", on ? 18 : 0);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  return (cat_read_line(sock, line, sizeof(line)) < 0) ? -1 : 0;
}

//
// AF gain (the Kenwood's speaker volume, 0..255): Kenwood "AG;" -> "AGnnn",
// "AGnnn;" sets it (checked on the TS-570D: AG080 / AG000 read back);
// rigctld "l AF" / "L AF" (0..1).
// Returns the gain, -1 if the connection failed, -2 if not available.
//
static int be_get_af(int sock) {
  char frame[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int rc = kw_query(sock, "AG;", "AG", frame, sizeof(frame));
    if (rc < 0) { return rc; }
    if (strlen(frame) < 5) { return -2; }
    return atoi(frame + 2);
  }
  const char *cmd = "l AF\n";
  if (send(sock, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t) strlen(cmd)) { return -1; }
  if (cat_read_line(sock, frame, sizeof(frame)) < 0) { return -1; }
  if (strncmp(frame, "RPRT", 4) == 0) { return -2; }
  return (int)(255.0 * atof(frame) + 0.5);
}

static int be_set_af(int sock, int af) {
  char cmd[32], line[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int len = snprintf(cmd, sizeof(cmd), "AG%03d;", af);
    return (send(sock, cmd, len, MSG_NOSIGNAL) == len) ? 0 : -1;
  }
  int len = snprintf(cmd, sizeof(cmd), "L AF %.3f\n", af / 255.0);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  return (cat_read_line(sock, line, sizeof(line)) < 0) ? -1 : 0;
}

static int be_set_preamp(int sock, int on) {
  char cmd[32], line[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int len = snprintf(cmd, sizeof(cmd), "PA%d;", on ? 1 : 0);
    return (send(sock, cmd, len, MSG_NOSIGNAL) == len) ? 0 : -1;
  }
  int len = snprintf(cmd, sizeof(cmd), "L PREAMP %d\n", on ? 12 : 0);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  return (cat_read_line(sock, line, sizeof(line)) < 0) ? -1 : 0;
}

//
// Output power in W: Kenwood "PC;" -> "PCnnn", rigctld "l RFPOWER" (0..1).
// Returns the power, -1 if the connection failed, -2 if not available.
//
static int be_get_pc(int sock) {
  char frame[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int rc = kw_query(sock, "PC;", "PC", frame, sizeof(frame));
    if (rc < 0) { return rc; }
    if (strlen(frame) < 5) { return -2; }
    return atoi(frame + 2);
  }
  const char *cmd = "l RFPOWER\n";
  if (send(sock, cmd, strlen(cmd), MSG_NOSIGNAL) != (ssize_t) strlen(cmd)) { return -1; }
  if (cat_read_line(sock, frame, sizeof(frame)) < 0) { return -1; }
  if (strncmp(frame, "RPRT", 4) == 0) { return -2; }
  return (int)(100.0 * atof(frame) + 0.5);
}

static int be_set_pc(int sock, int w) {
  char cmd[32], line[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    int len = snprintf(cmd, sizeof(cmd), "PC%03d;", w);
    return (send(sock, cmd, len, MSG_NOSIGNAL) == len) ? 0 : -1;
  }
  int len = snprintf(cmd, sizeof(cmd), "L RFPOWER %.2f\n", 0.01 * w);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  return (cat_read_line(sock, line, sizeof(line)) < 0) ? -1 : 0;
}

//
// Set the Kenwood mode (index into ifpan_mode_names[]): Kenwood "MDn;" (no
// answer), rigctld "M <mode> 0" (reply "RPRT n"). Returns -1 if the
// connection failed.
//
static int be_set_mode(int sock, int m) {
  char cmd[32], line[64];
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    // same order as ifpan_mode_names[]: USB LSB CW CW-R AM FM FSK FSKR
    static const int md[8] = { 2, 1, 3, 7, 5, 4, 6, 9 };
    int len = snprintf(cmd, sizeof(cmd), "MD%d;", md[m]);
    return (send(sock, cmd, len, MSG_NOSIGNAL) == len) ? 0 : -1;
  }
  int len = snprintf(cmd, sizeof(cmd), "M %s 0\n", hamlib_mode_names[m]);
  if (send(sock, cmd, len, MSG_NOSIGNAL) != len) { return -1; }
  if (cat_read_line(sock, line, sizeof(line)) < 0) { return -1; }
  if (strncmp(line, "RPRT 0", 6) != 0) {
    t_print("%s: rigctld refused %s", __func__, cmd);
  }
  return 0;
}

static int be_get_strength(int sock, int *db) {
  return (ifpan_cat_backend == IFPAN_CAT_KENWOOD) ? kw_get_strength(sock, db) : cat_get_strength(sock, db);
}

//
// PTT: Kenwood "TX;" / "RX;" (no answer), rigctld "T 1" / "T 0" (reply
// "RPRT n"). Returns -1 if the connection failed.
//
static int be_set_ptt(int sock, int on) {
  if (ifpan_cat_backend == IFPAN_CAT_KENWOOD) {
    const char *cmd = on ? "TX;" : "RX;";
    return (send(sock, cmd, 3, MSG_NOSIGNAL) == 3) ? 0 : -1;
  }
  char line[64];
  const char *cmd = on ? "T 1\n" : "T 0\n";
  if (send(sock, cmd, 4, MSG_NOSIGNAL) != 4) { return -1; }
  if (cat_read_line(sock, line, sizeof(line)) < 0) { return -1; }
  if (strncmp(line, "RPRT 0", 6) != 0) {
    t_print("%s: rigctld refused %s", __func__, cmd);
  }
  return 0;
}

//
// While a frequency set is on its way to the Kenwood: wait at most
// ifpan.poll_ms before reading again, but go on at once when a newer target
// is waiting. A fixed 50 ms sleep here made the CAT cycle 120-150 ms while
// a knob or the mouse wheel was turning (ifpan-tunelog, 2026-10-13): the
// Kenwood - and so the IF on the display - moved only 7-8 times a second,
// in jumps of up to 2 kHz.
//
static void hold_wait(void) {
  gint64 end = g_get_monotonic_time() + (gint64)ifpan_poll_ms * 1000;
  while (g_get_monotonic_time() < end) {
    long long pending;
    g_mutex_lock(&cat_mutex);
    pending = cat_set_target;
    g_mutex_unlock(&cat_mutex);
    if (pending > 0) { return; }
    usleep(10000);
  }
}

static gpointer cat_thread(gpointer data) {
  int sock = -1;
  long long hold_freq = 0;            // frequency just sent to the Kenwood
  gint64 hold_until = 0;              // until then, ignore other readings
  int hold_resends = 0;
  gint64 last_set_us = 0;             // when the last frequency set went out               // times the target was sent again (dropped by the Kenwood)
  int trouble = 0;                    // a CAT problem has been reported (log once)
  int mode_count = 0;                 // polls until the next mode read (0: now)
  int smeter_count = 0;               // polls until the next S-meter read
  int atu_count = 0;                  // polls until the next antenna tuner read
  int silent_polls = 0;               // consecutive polls without an answer
  int want_mode = -1;                 // mode still to be set on the Kenwood (band change)
  int want_tries = 0;
  gint64 want_at = 0;
  t_print("%s: CAT via %s (%s:%d), IF=%lld Hz\n", __func__, cat_name(),
          ifpan_cat_backend == IFPAN_CAT_KENWOOD ? ifpan_kenwood_host : ifpan_rigctld_host,
          ifpan_cat_backend == IFPAN_CAT_KENWOOD ? ifpan_kenwood_port : ifpan_rigctld_port, ifpan_if_freq);
  for (;;) {
    if (cat_reconnect) {
      //
      // CAT settings changed in the menu: drop the link, start again
      //
      cat_reconnect = 0;
      if (sock >= 0) {
        close(sock);
        sock = -1;
      }
      trouble = 0;
      ifpan_cat_connected = 0;
      ifpan_kenwood_mode = -1;
      ifpan_kenwood_smeter = -999;
      ifpan_kenwood_preamp = -1;
      ifpan_kenwood_att = -1;
      ifpan_kenwood_atu = -1;
      ifpan_kenwood_atu_tuning = 0;
      g_idle_add(ext_vfo_update, NULL);
      t_print("%s: CAT now via %s (%s:%d)\n", __func__, cat_name(),
              ifpan_cat_backend == IFPAN_CAT_KENWOOD ? ifpan_kenwood_host : ifpan_rigctld_host,
              ifpan_cat_backend == IFPAN_CAT_KENWOOD ? ifpan_kenwood_port : ifpan_rigctld_port);
    }
    if (sock < 0) {
      //
      // At most one connection attempt per second: when the serial adapter
      // behind the CAT bridge drops off USB (seen 2026-10-13: the USB hub
      // reset while transmitting), the bridge still accepts TCP connections
      // but closes them at once, and this loop reconnected thousands of
      // times a second (3.4 MB of log in seconds).
      //
      static gint64 last_connect_us = 0;
      gint64 since = g_get_monotonic_time() - last_connect_us;
      if (since < 1000000) { usleep((useconds_t)(1000000 - since)); }
      last_connect_us = g_get_monotonic_time();
      sock = cat_connect();
      if (sock < 0) {
        if (!trouble) {
          t_print("%s: cannot connect to %s\n", __func__, cat_name());
          trouble = 1;
          ifpan_cat_connected = 0;
          g_idle_add(ext_vfo_update, NULL);
        }
        usleep(1000000);
        continue;
      }
      if (be_setup(sock) < 0) {
        // rigctld accepted the connection but is stuck opening the radio
        if (!trouble) {
          t_print("%s: %s not answering (radio off, wrong baud rate or cable?)\n", __func__, cat_name());
          trouble = 1;
          ifpan_cat_connected = 0;
          g_idle_add(ext_vfo_update, NULL);
        }
        close(sock);
        sock = -1;
        usleep(1000000);
        continue;
      }
      t_print("%s: connected (%s)\n", __func__, cat_name());
    }
    //
    // piHPSDR-IF -> Kenwood: send the latest requested frequency, if any
    //
    long long target;
    g_mutex_lock(&cat_mutex);
    target = cat_set_target;
    cat_set_target = 0;
    g_mutex_unlock(&cat_mutex);
    if (target > 0) {
      if (be_set_freq(sock, target) < 0) {
        t_print("%s: %s connection lost\n", __func__, cat_name());
        ifpan_cat_connected = 0;
        g_idle_add(ext_vfo_update, NULL);
        close(sock);
        sock = -1;
        continue;
      }
      TUNELOG("FA set %lld\n", target);
      hold_freq = target;
      hold_resends = 0;
      last_set_us = g_get_monotonic_time();
      hold_until = last_set_us + CAT_SET_HOLD_US;
      //
      // Trust the set: the display centre follows it at once (the Kenwood is
      // there a few ms later); a reading after the knob stops confirms it,
      // and the resend logic below corrects a command the Kenwood dropped.
      //
      g_mutex_lock(&cat_mutex);
      centre_pending = target;
      if (!centre_scheduled) {
        centre_scheduled = 1;
        g_idle_add(centre_apply, NULL);
      }
      g_mutex_unlock(&cat_mutex);
    }
    //
    // piHPSDR-IF band change -> Kenwood mode (index into ifpan_mode_names[]).
    // A mode command sent right after the band-changing frequency command
    // got lost (the TS-570 is busy switching bands and recalls its own band
    // memory), so it is sent 300 ms later, read back, and repeated if it did
    // not take (up to 5 times).
    //
    g_mutex_lock(&cat_mutex);
    if (cat_set_mode >= 0) {
      want_mode = cat_set_mode;
      want_tries = 0;
      want_at = g_get_monotonic_time() + 300000;
      cat_set_mode = -1;
    }
    g_mutex_unlock(&cat_mutex);
    if (want_mode >= 0 && g_get_monotonic_time() >= want_at) {
      int m = be_set_mode(sock, want_mode);
      if (m == 0) {
        usleep(50000);
        m = be_get_mode(sock);        // read back: index, -1 link lost, -2/-3 no / other answer
      }
      if (m == -1) {
        t_print("%s: %s connection lost\n", __func__, cat_name());
        ifpan_cat_connected = 0;
        g_idle_add(ext_vfo_update, NULL);
        close(sock);
        sock = -1;
        continue;
      }
      t_print("%s: mode %s sent to the Kenwood, read back %s\n", __func__, ifpan_mode_names[want_mode],
              m >= 0 ? ifpan_mode_names[m] : "nothing");
      if (m == want_mode || ++want_tries >= 5) {
        if (m != want_mode) { t_print("%s: Kenwood did not take mode %s\n", __func__, ifpan_mode_names[want_mode]); }
        want_mode = -1;
        mode_count = 0;               // read it back now (TS readout)
      } else {
        want_at = g_get_monotonic_time() + 300000;
      }
    }
    //
    // TUNE start (before its PTT): mode for the ACC2 tone, tune power
    //
    int tr, tw, tdirty;
    g_mutex_lock(&cat_mutex);
    tr = tune_request;
    tune_request = -1;
    tw = tune_watts;
    tdirty = tune_pc_dirty;
    tune_pc_dirty = 0;
    g_mutex_unlock(&cat_mutex);
    if (tr == 1 && !tune_active) {
      int m = ifpan_kenwood_mode;
      int swap = (m == 2 || m == 6) ? 0 : (m == 3 || m == 7) ? 1 : -1;  // CW/FSK -> USB, CW-R/FSKR -> LSB
      tune_active = 1;
      tune_stopping = 0;
      tune_saved_mode = -1;
      if (swap >= 0) {
        if (be_set_mode(sock, swap) < 0) {
          close(sock);
          sock = -1;
          continue;
        }
        tune_saved_mode = m;
        usleep(50000);
      }
      tune_saved_pc = be_get_pc(sock);
      if (tune_saved_pc == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      tdirty = 1;
      t_print("%s: %s at %d W%s%s\n", __func__, tune_is_twotone ? "TwoTone" : "TUNE", tw, swap >= 0 ? ", Kenwood to " : "",
              swap >= 0 ? ifpan_mode_names[swap] : "");
    }
    if (tr == 0 && tune_active) { tune_stopping = 1; }
    if (tune_active && !tune_stopping && tdirty) {
      if (be_set_pc(sock, tw) < 0) {
        close(sock);
        sock = -1;
        continue;
      }
    }
    //
    // piHPSDR-IF PTT (toolbar PTT / MOX) -> Kenwood
    //
    int ptt;
    g_mutex_lock(&cat_mutex);
    ptt = ptt_request;
    ptt_request = -1;
    g_mutex_unlock(&cat_mutex);
    if (ptt >= 0) {
      if (be_set_ptt(sock, ptt) < 0) {
        if (ptt == 0) {               // RX must not get lost
          g_mutex_lock(&cat_mutex);
          if (ptt_request < 0) { ptt_request = 0; }
          g_mutex_unlock(&cat_mutex);
        }
        t_print("%s: %s connection lost\n", __func__, cat_name());
        ifpan_cat_connected = 0;
        g_idle_add(ext_vfo_update, NULL);
        close(sock);
        sock = -1;
        continue;
      }
      ptt_keyed = ptt;
      t_print("%s: PTT %s sent to the Kenwood\n", __func__, ptt ? "TX" : "RX");
      if (!ptt) { txlev_report(); }
    }
    //
    // TUNE end, once the Kenwood is back in RX: if its mode was switched,
    // that mode's own PWR setting and the mode back; then the TX Drive
    // power (also when the slider was moved during the tune)
    //
    if (tune_stopping && !ptt_keyed) {
      int dw;
      g_mutex_lock(&cat_mutex);
      dw = tune_drive_watts;
      g_mutex_unlock(&cat_mutex);
      if (tune_saved_mode >= 0) {
        if (tune_saved_pc >= 0 && be_set_pc(sock, tune_saved_pc) < 0) {
          close(sock);
          sock = -1;
          continue;
        }
        usleep(50000);
        if (be_set_mode(sock, tune_saved_mode) < 0) {
          close(sock);
          sock = -1;
          continue;
        }
        usleep(50000);
      }
      if (be_set_pc(sock, dw) < 0) {
        close(sock);
        sock = -1;
        continue;
      }
      t_print("%s: TUNE done, Kenwood PWR %d W%s%s\n", __func__, dw,
              tune_saved_mode >= 0 ? ", mode " : "", tune_saved_mode >= 0 ? ifpan_mode_names[tune_saved_mode] : "");
      tune_active = 0;
      tune_stopping = 0;
      tune_saved_mode = -1;
      tune_saved_pc = -1;
      mode_count = 0;
    }
    //
    // While the knob / wheel keeps sending targets, do not read the Kenwood
    // back after every set (each reading takes the TS-570 tens of ms and
    // limited the sets to 7-8 a second): wait up to TRUST_SET_US after the
    // last set for the next target (or PTT etc.) and only read once the
    // tuning pauses.
    //
    if (hold_until != 0) {
      int busy = 0;
      while (g_get_monotonic_time() < last_set_us + TRUST_SET_US) {
        long long pending;
        int ptt_pending;
        g_mutex_lock(&cat_mutex);
        pending = cat_set_target;
        ptt_pending = ptt_request;
        g_mutex_unlock(&cat_mutex);
        if (pending > 0 || ptt_pending >= 0 || cat_reconnect) { busy = 1; break; }
        usleep(5000);
      }
      if (busy) { continue; }
    }
    long long f = be_get_freq(sock);
    if (f > 0) { TUNELOG("TS read %lld%s\n", f, hold_until ? " (set on its way)" : ""); }
    if (f == -2) {
      //
      // rigctld is there but the radio does not answer (off, wrong baud
      // rate, cable): say so once, grey out "TS", and try again calmly.
      //
      //
      // Right after a burst of frequency sets (knob, mouse wheel) the TS-570
      // sometimes leaves one reading unanswered: only two in a row mean
      // "not answering" (before, one missed reading greyed out "TS", stopped
      // the tuning steps and paused 0.5 s: ~1 s of stalled tuning).
      //
      ++silent_polls;
      TUNELOG("TS no answer (%d in a row)\n", silent_polls);
      if (silent_polls >= 2 && !trouble) {
        t_print("%s: Kenwood not answering (radio off, wrong baud rate or cable?)\n", __func__);
        trouble = 1;
        ifpan_cat_connected = 0;
        g_idle_add(ext_vfo_update, NULL);
      }
      //
      // Kenwood TCP: after ~3 s without an answer, open a new connection.
      // A serial bridge (socat) whose USB-RS232 adapter was unplugged and
      // came back keeps the old, dead device open per connection; a new
      // connection makes it open the adapter again.
      //
      if (ifpan_cat_backend == IFPAN_CAT_KENWOOD && silent_polls >= 6) {
        t_print("%s: no answer for 3 s, reconnecting\n", __func__);
        silent_polls = 0;
        close(sock);
        sock = -1;
      }
      // pause before asking again, but not while tuning steps are waiting
      {
        gint64 end = g_get_monotonic_time() + 500000;
        while (g_get_monotonic_time() < end) {
          long long pending;
          g_mutex_lock(&cat_mutex);
          pending = cat_set_target;
          g_mutex_unlock(&cat_mutex);
          if (pending > 0) { break; }
          usleep(10000);
        }
      }
      continue;
    }
    silent_polls = 0;
    if (f < 0) {
      if (!trouble) {
        t_print("%s: %s connection lost\n", __func__, cat_name());
        trouble = 1;
        ifpan_cat_connected = 0;
        g_idle_add(ext_vfo_update, NULL);
      }
      close(sock);
      sock = -1;
      continue;
    }
    if (trouble || !ifpan_cat_connected) {
      t_print("%s: Kenwood answering (%lld Hz)\n", __func__, f);
      trouble = 0;
      ifpan_cat_connected = 1;
      mode_count = 0;
      g_idle_add(ext_vfo_update, NULL);
    }
    //
    // Kenwood mode, shown next to "TS": changes rarely, so read it only
    // every CAT_MODE_EVERY polls (each read keeps the radio busy ~30 ms)
    //
    if (mode_count-- <= 0) {
      mode_count = CAT_MODE_EVERY;
      int m = be_get_mode(sock);
      if (m == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      if (m == -3) { m = -1; }
      // (not while TUNE has switched the Kenwood to USB / LSB for the tone)
      if (m >= -1 && m != ifpan_kenwood_mode && tune_saved_mode < 0) {
        ifpan_kenwood_mode = m;
        g_idle_add(ext_vfo_update, NULL);
        if (m >= 0) { g_idle_add(mode_apply, GINT_TO_POINTER(m)); }
        g_idle_add(ifpan_retune_sdr, NULL);   // the IF offset of this mode
      }
      // preamp state (also when switched on the radio itself)
      int p = be_get_preamp(sock);
      if (p == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      if (p < 0) { p = -1; }
      if (p != ifpan_kenwood_preamp) {
        ifpan_kenwood_preamp = p;
        g_idle_add(ext_vfo_update, NULL);
      }
      // attenuator state, likewise
      p = be_get_att(sock);
      if (p == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      if (p < 0) { p = -1; }
      if (p != ifpan_kenwood_att) {
        ifpan_kenwood_att = p;
        g_idle_add(ext_vfo_update, NULL);
      }
      // while TS MUTE holds the AF gain at 0, VOL turned up on the radio ends it
      if (ifpan_ts_muted) {
        int af = be_get_af(sock);
        if (af == -1) {
          close(sock);
          sock = -1;
          continue;
        }
        if (af > 0) {
          ifpan_ts_muted = 0;
          t_print("%s: TS VOL turned (%d): mute ended\n", __func__, af);
          g_idle_add(ext_vfo_update, NULL);
        }
      }
      // PWR setting (per mode on the radio): the TX Drive slider follows
      p = be_get_pc(sock);
      if (p == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      if (p < 0) { p = -1; }
      if (!tune_active) { ifpan_kenwood_pc = p; }   // (the client follows it too)
      if (p >= 0 && !tune_active && transmitter != NULL && p != (int)(transmitter->drive + 0.5)) {
        g_idle_add(ifpan_power_apply, GINT_TO_POINTER(p));
      }
    }
    // TX Drive slider -> Kenwood PWR, read back at once
    int pc;
    g_mutex_lock(&cat_mutex);
    pc = pc_request;
    pc_request = -1;
    g_mutex_unlock(&cat_mutex);
    if (pc >= 0) {
      if (be_set_pc(sock, pc) < 0) {
        close(sock);
        sock = -1;
        continue;
      }
      mode_count = 0;
    }
    //
    // toolbar PRE -> Kenwood preamp, read back at once
    //
    int pre;
    g_mutex_lock(&cat_mutex);
    pre = preamp_request;
    preamp_request = -1;
    g_mutex_unlock(&cat_mutex);
    if (pre >= 0) {
      if (be_set_preamp(sock, pre) < 0) {
        close(sock);
        sock = -1;
        continue;
      }
      mode_count = 0;
    }
    // toolbar TS MUTE -> Kenwood AF gain to 0, or back to its saved level
    int mreq;
    g_mutex_lock(&cat_mutex);
    mreq = ts_mute_request;
    ts_mute_request = -1;
    g_mutex_unlock(&cat_mutex);
    if (mreq == 1 && !ifpan_ts_muted) {
      int af = be_get_af(sock);
      if (af == -1 || (af >= 0 && be_set_af(sock, 0) < 0)) {
        close(sock);
        sock = -1;
        continue;
      }
      if (af >= 0) {
        ts_af_saved = af;
        ifpan_ts_muted = 1;
        t_print("%s: TS muted (AF gain was %d)\n", __func__, af);
        g_idle_add(ext_vfo_update, NULL);
      }
    } else if (mreq == 0 && ifpan_ts_muted) {
      if (be_set_af(sock, ts_af_saved) < 0) {
        close(sock);
        sock = -1;
        continue;
      }
      ifpan_ts_muted = 0;
      t_print("%s: TS unmuted (AF gain %d)\n", __func__, ts_af_saved);
      g_idle_add(ext_vfo_update, NULL);
    }
    // toolbar ATT -> Kenwood attenuator, read back at once
    int att;
    g_mutex_lock(&cat_mutex);
    att = att_request;
    att_request = -1;
    g_mutex_unlock(&cat_mutex);
    if (att >= 0) {
      if (be_set_att(sock, att) < 0) {
        close(sock);
        sock = -1;
        continue;
      }
      mode_count = 0;
    }
    //
    // antenna tuner: toolbar request, then its state - with the mode
    // (about once a second), and on every poll while it is tuning
    //
    int atu;
    g_mutex_lock(&cat_mutex);
    atu = atu_request;
    atu_request = -1;
    g_mutex_unlock(&cat_mutex);
    if (atu >= 0) {
      if (be_set_atu(sock, atu) < 0) {
        close(sock);
        sock = -1;
        continue;
      }
      atu_count = 0;
      usleep(50000);                  // let the radio switch before reading back
    }
    if (atu_count-- <= 0 || ifpan_kenwood_atu_tuning) {
      atu_count = CAT_MODE_EVERY;
      int a = be_get_atu(sock);
      if (a == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      int on = (a >= 0) ? (a & 1) : -1;
      int tuning = (a >= 0) ? ((a >> 1) & 1) : 0;
      if (on != ifpan_kenwood_atu || tuning != ifpan_kenwood_atu_tuning) {
        if (tuning != ifpan_kenwood_atu_tuning) {
          t_print("%s: Kenwood antenna tuner %s\n", __func__, tuning ? "tuning" : "done");
        }
        ifpan_kenwood_atu = on;
        ifpan_kenwood_atu_tuning = tuning;
        g_idle_add(ext_vfo_update, NULL);
      }
    }
    //
    // Kenwood S-meter, shown next to the piHPSDR-IF meter (the meter is
    // redrawn continuously, so only the value is stored here); while the
    // Kenwood transmits, its TX meters instead (Kenwood TCP only)
    //
    if (ifpan_cat_backend == IFPAN_CAT_KENWOOD && kw_tx_meters(sock) < 0) {
      close(sock);
      sock = -1;
      continue;
    }
    if (!ifpan_kenwood_tx && smeter_count-- <= 0) {
      smeter_count = CAT_SMETER_EVERY;
      int db;
      int rc = be_get_strength(sock, &db);
      if (rc == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      ifpan_kenwood_smeter = (rc == 0) ? db : -999;
    }
    //
    // Right after a set, the radio may still report the old frequency:
    // ignore such readings so the display does not jump back. The Kenwood
    // rounds to 10 Hz, so accept anything within 10 Hz of the target.
    //
    if (hold_until != 0) {
      if (llabs(f - hold_freq) >= 10 && f > 0 && f != vfo[VFO_A].frequency) {
        // not there yet, but this is where the Kenwood (and so the IF) is
        // now: the display centre follows it (centre_only, main thread)
        g_mutex_lock(&cat_mutex);
        centre_pending = f;
        if (!centre_scheduled) {
          centre_scheduled = 1;
          g_idle_add(centre_apply, NULL);
        }
        g_mutex_unlock(&cat_mutex);
      }
      if (llabs(f - hold_freq) < 10) {
        hold_until = 0;
        hold_resends = 0;
      } else if (g_get_monotonic_time() < hold_until) {
        hold_wait();
        continue;
      } else if (hold_resends < 3) {
        //
        // The Kenwood still is elsewhere: when piHPSDR-IF tunes fast (mouse
        // wheel), the TS-570 drops commands that arrive while it is busy,
        // and following its stale frequency made the display jump back and
        // forth. Send the target again (up to 3 times) before following it.
        //
        hold_resends++;
        if (be_set_freq(sock, hold_freq) < 0) {
          close(sock);
          sock = -1;
          continue;
        }
        hold_until = g_get_monotonic_time() + CAT_SET_HOLD_US;
        hold_wait();
        continue;
      } else {
        hold_until = 0;
        hold_resends = 0;
      }
    }
    //
    // Compare with the display centre rather than with the previous
    // reading, so that anything that moved the centre away (band
    // menu, memories, ...) is put right again on the next poll.
    //
    if (f != vfo[VFO_A].frequency) {
      g_mutex_lock(&cat_mutex);
      cat_pending = f;
      if (!cat_scheduled) {
        cat_scheduled = 1;
        g_idle_add(cat_apply, NULL);
      }
      g_mutex_unlock(&cat_mutex);
    }
    //
    // How fast the display follows the Kenwood: poll interval =
    // ifpan.poll_ms (50 ms). (Until 2026-10-12 it was multiplied by the
    // "VFO Encoder Divisor", which is now needed for a MIDI VFO encoder:
    // a divisor of 60 made the Kenwood readings 3 s apart.) A pending
    // piHPSDR-IF -> Kenwood request ends the wait at once, so tuning here
    // is never delayed.
    //
    gint64 wait_end = g_get_monotonic_time() + (gint64)ifpan_poll_ms * 1000;
    while (g_get_monotonic_time() < wait_end) {
      long long pending;
      int ptt_pending;
      g_mutex_lock(&cat_mutex);
      pending = cat_set_target;
      ptt_pending = ptt_request;
      g_mutex_unlock(&cat_mutex);
      if (pending > 0 || ptt_pending >= 0 || cat_reconnect) { break; }
      usleep(10000);
    }
  }
  return NULL;
}

//
// Window position: remembered in a small file of its own (written 1 s after
// the window stops moving), so it also survives a killed program. Only works
// where the window system lets clients place windows (X11, XWayland); under
// Wayland the position always reads 0,0 and nothing is saved.
//
#define WINDOW_POS_FILE "ifpan-window.txt"
static int win_x = 0, win_y = 0;
static guint win_save_timer = 0;

static gboolean win_save(gpointer data) {
  win_save_timer = 0;
  FILE *fp = fopen(WINDOW_POS_FILE, "w");
  if (fp) {
    fprintf(fp, "%d %d\n", win_x, win_y);
    fclose(fp);
  }
  return G_SOURCE_REMOVE;
}

static int win_tracking = 0;          // save moves only after the restore is done
static int win_have_saved = 0;

//
// A position is only used / saved if the window's top-left area is on a
// monitor. While the window is minimised, Windows reports a position far
// off-screen (about -32000, i.e. -16000 at 200 % scaling): once saved (or
// "compensated" in win_apply), every start put the window out of sight.
//
static int win_on_screen(int x, int y) {
  GdkDisplay *d = gdk_display_get_default();
  if (d == NULL) { return 1; }
  int n = gdk_display_get_n_monitors(d);
  for (int i = 0; i < n; i++) {
    GdkRectangle r;
    gdk_monitor_get_geometry(gdk_display_get_monitor(d, i), &r);
    if (x + 50 >= r.x && x + 50 < r.x + r.width && y + 20 >= r.y && y + 20 < r.y + r.height) { return 1; }
  }
  return 0;
}

static int win_iconified(void) {
  GdkWindow *w = gtk_widget_get_window(top_window);
  return w != NULL && (gdk_window_get_state(w) & GDK_WINDOW_STATE_ICONIFIED);
}

static gboolean win_moved(GtkWidget *widget, GdkEvent *event, gpointer data) {
  int x, y;
  if (!win_tracking || win_iconified()) { return FALSE; }
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  if ((x != 0 || y != 0) && (x != win_x || y != win_y) && win_on_screen(x, y)) {
    win_x = x;
    win_y = y;
    if (win_save_timer) { g_source_remove(win_save_timer); }
    win_save_timer = g_timeout_add(1000, win_save, NULL);
  }
  return FALSE;
}

//
// The window manager places a new window itself when it is mapped, so the
// saved position is applied some time after start-up, and once more if the
// window manager moved it again. Only then are moves tracked and saved.
//
static gboolean win_apply(gpointer data) {
  int pass = GPOINTER_TO_INT(data);
  int x, y;
  gtk_window_get_position(GTK_WINDOW(top_window), &x, &y);
  if (win_have_saved && pass == 0) {
    gtk_window_move(GTK_WINDOW(top_window), win_x, win_y);
  } else if (win_have_saved && pass == 1 && (x != win_x || y != win_y) && !win_iconified()
             && win_on_screen(2 * win_x - x, 2 * win_y - y)) {
    //
    // Some window managers place the frame, others the client area, so the
    // position read back may differ from the one requested by the frame
    // size: compensate once (not if minimised: that position is fictitious).
    //
    gtk_window_move(GTK_WINDOW(top_window), 2 * win_x - x, 2 * win_y - y);
  }
  if (pass < 2) {
    g_timeout_add(1000, win_apply, GINT_TO_POINTER(pass + 1));
  } else {
    // from now on, only moves by the user are saved
    t_print("%s: window at %d,%d (saved %d,%d)\n", __func__, x, y, win_x, win_y);
    if (win_on_screen(x, y)) {
      win_x = x;
      win_y = y;
    }
    win_tracking = 1;
  }
  return G_SOURCE_REMOVE;
}

static void win_restore(void) {
  int x, y;
  FILE *fp = fopen(WINDOW_POS_FILE, "r");
  if (fp) {
    if (fscanf(fp, "%d %d", &x, &y) == 2) {
      // some window managers refuse negative positions: use the screen edge
      win_x = (x < 0) ? 0 : x;
      win_y = (y < 0) ? 0 : y;
      // a position off all monitors (saved while minimised) is not used
      win_have_saved = win_on_screen(win_x, win_y);
      if (!win_have_saved) { t_print("%s: saved position %d,%d is off-screen, not used\n", __func__, x, y); }
    }
    fclose(fp);
  }
  g_signal_connect(top_window, "configure-event", G_CALLBACK(win_moved), NULL);
  g_timeout_add(1500, win_apply, GINT_TO_POINTER(0));
}

void ifpan_start(void) {
  if (cat_thread_id != NULL) { return; }
  //
  // The SDR only ever receives the IF; the frequencies shown and tuned are
  // the Kenwood's, so the band menu and the frequency checks use the
  // TS-570 coverage instead of the SDR's (an RTL-SDR reports 24 MHz and up,
  // which hid all HF bands).
  //
  radio->frequency_min = IFPAN_RADIO_FMIN;
  radio->frequency_max = IFPAN_RADIO_FMAX;
  cat_thread_id = g_thread_new("ifpan_cat", cat_thread, NULL);
  g_timeout_add(50, ui_watch, NULL);   // 20 tuning steps a second: the most the TS-570 takes (25 ms = ~38/s made it
                                       // drop commands and go silent for seconds, ifpan-tunelog 2026-10-13)
  win_restore();
  ifpan_tx_out_select(ifpan_tx_out);   // processed TX audio to the Kenwood's ACC2
  txdump_on = (access("ifpan-txdump", F_OK) == 0);
  tunelog_on = (access("ifpan-tunelog", F_OK) == 0);
  if (tunelog_on) { t_print("%s: tuning diagnostics on (ifpan-tunelog)\n", __func__); }
  if (txdump_on) { t_print("%s: TX audio diagnostics on (ifpan-txdump)\n", __func__); }
}

//
// Client of a piHPSDR-IF server: no CAT link of its own (the server owns
// the Kenwood and sends its state, see INFO_IFPAN); only the window
// position is remembered, as on the server.
//
void ifpan_client_start(void) {
  static int started = 0;
  if (started) { return; }
  started = 1;
  win_restore();
}
