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

#include "ext.h"
#include "ifpan.h"
#include "main.h"
#include "message.h"
#include "mode.h"
#include "property.h"
#include "radio.h"
#include "receiver.h"
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
  if (!ifpan_cat_connected) { return; }
  g_mutex_lock(&cat_mutex);
  preamp_request = (ifpan_kenwood_preamp == 1) ? 0 : 1;
  g_mutex_unlock(&cat_mutex);
}

static int       att_request = -1;    // toolbar ATT: 0 / 1 to send, -1: none
int              ifpan_kenwood_att = -1;      // TS attenuator read by CAT: 0 / 1, -1 unknown

// toolbar ATT (main thread): toggle the Kenwood attenuator
void ifpan_att_toggle(void) {
  if (!ifpan_cat_connected) { return; }
  g_mutex_lock(&cat_mutex);
  att_request = (ifpan_kenwood_att == 1) ? 0 : 1;
  g_mutex_unlock(&cat_mutex);
}
static long long ui_last_freq = -1;   // receiver frequency last seen (main thread only)
static int       ui_last_mode = -1;   // receiver mode last seen (main thread only)

//
// Frequency the PlutoSDR LO is tuned to: the IF centre plus the LO offset
// (with the radio's frequency calibration applied). The offset moves the
// Pluto's DC spike away from the Kenwood VFO; soapy_protocol.c shifts the
// samples back digitally so the IF centre stays in the middle of the display.
//
long long ifpan_hw_frequency(void) {
  return calibrated_frequency(ifpan_if_freq + ifpan_lo_offset);
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
  if (ifpan_cat_backend != IFPAN_CAT_KENWOOD) { ifpan_cat_backend = IFPAN_CAT_RIGCTLD; }
  if (ifpan_poll_ms < 20) { ifpan_poll_ms = 20; }
  // keep the IF well inside the Pluto span (+/- 384 kHz at 768 kHz)
  if (ifpan_lo_offset >  200000) { ifpan_lo_offset =  200000; }
  if (ifpan_lo_offset < -200000) { ifpan_lo_offset = -200000; }
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
static gboolean mode_apply(gpointer data) {
  // same order as ifpan_mode_names[]: USB LSB CW CW-R AM FM FSK FSKR
  static const int to_mode[8] = { modeUSB, modeLSB, modeCWU, modeCWL, modeAM, modeFMN, modeDIGL, modeDIGU };
  int m = GPOINTER_TO_INT(data);
  if (!ifpan_follow_mode || m < 0 || m > 7 || receivers < 1 || receiver[0] == NULL) { return G_SOURCE_REMOVE; }
  if (vfo[VFO_A].mode != to_mode[m]) {
    t_print("%s: Kenwood %s -> receiver %s\n", __func__, ifpan_mode_names[m], mode_string[to_mode[m]]);
    vfo_id_mode_changed(VFO_A, to_mode[m]);
    ui_last_mode = to_mode[m];        // came from the Kenwood: not to be sent back
  }
  return G_SOURCE_REMOVE;
}

//
// piHPSDR mode -> index into ifpan_mode_names[] (the Kenwood's modes),
// -1 for modes the Kenwood does not have (DSB, SAM, SPEC, DRM)
//
static int kenwood_mode_index(int mode) {
  switch (mode) {
  case modeUSB:  return 0;
  case modeLSB:  return 1;
  case modeCWU:  return 2;
  case modeCWL:  return 3;
  case modeAM:   return 4;
  case modeFMN:  return 5;
  case modeDIGL: return 6;
  case modeDIGU: return 7;
  default:       return -1;
  }
}

// Radio menu: switching "Follow TS mode" on applies the Kenwood mode at once
void ifpan_set_follow_mode(int on) {
  ifpan_follow_mode = on;
  if (on && ifpan_kenwood_mode >= 0) { mode_apply(GINT_TO_POINTER(ifpan_kenwood_mode)); }
}

static gboolean cat_apply(gpointer data) {
  long long f;
  g_mutex_lock(&cat_mutex);
  f = cat_pending;
  cat_scheduled = 0;
  g_mutex_unlock(&cat_mutex);
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
  if (on && receivers > 0 && receiver[0] != NULL && vfo[VFO_A].ctun) {
    vfo[VFO_A].ctun_frequency = vfo[VFO_A].frequency;
    rx_frequency_changed(receiver[0]);
    ui_last_freq = vfo[VFO_A].ctun_frequency;   // not to be echoed to the Kenwood
    g_idle_add(ext_vfo_update, NULL);
  }
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
    ui_last_freq = f;
    if (ifpan_cat_write && ifpan_cat_connected) {
      g_mutex_lock(&cat_mutex);
      cat_set_target = f;
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
  if (!ifpan_cat_connected || receivers < 1 || receiver[0] == NULL) { return; }
  long long f = vfo[VFO_A].ctun ? vfo[VFO_A].ctun_frequency : vfo[VFO_A].frequency;
  g_mutex_lock(&cat_mutex);
  cat_set_target = f;
  g_mutex_unlock(&cat_mutex);
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
// On exit: if piHPSDR-IF keyed the Kenwood, put it back to receive
// (wait up to 1 s for the CAT thread to send it)
//
void ifpan_stop(void) {
  if (!ptt_keyed) { return; }
  ifpan_ptt(0);
  for (int i = 0; i < 100 && ptt_keyed; i++) { usleep(10000); }
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

static int kw_tx_meters(int sock) {
  char frame[64];
  char cmd[8];
  if (!ifpan_kenwood_tx) {
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
    if (sel == 1) { ifpan_kenwood_swr = val; }
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

static gpointer cat_thread(gpointer data) {
  int sock = -1;
  long long hold_freq = 0;            // frequency just sent to the Kenwood
  gint64 hold_until = 0;              // until then, ignore other readings
  int trouble = 0;                    // a CAT problem has been reported (log once)
  int mode_count = 0;                 // polls until the next mode read (0: now)
  int smeter_count = 0;               // polls until the next S-meter read
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
      g_idle_add(ext_vfo_update, NULL);
      t_print("%s: CAT now via %s (%s:%d)\n", __func__, cat_name(),
              ifpan_cat_backend == IFPAN_CAT_KENWOOD ? ifpan_kenwood_host : ifpan_rigctld_host,
              ifpan_cat_backend == IFPAN_CAT_KENWOOD ? ifpan_kenwood_port : ifpan_rigctld_port);
    }
    if (sock < 0) {
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
      hold_freq = target;
      hold_until = g_get_monotonic_time() + CAT_SET_HOLD_US;
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
    }
    long long f = be_get_freq(sock);
    if (f == -2) {
      //
      // rigctld is there but the radio does not answer (off, wrong baud
      // rate, cable): say so once, grey out "TS", and try again calmly.
      //
      if (!trouble) {
        t_print("%s: Kenwood not answering (radio off, wrong baud rate or cable?)\n", __func__);
        trouble = 1;
        ifpan_cat_connected = 0;
        g_idle_add(ext_vfo_update, NULL);
      }
      usleep(500000);
      continue;
    }
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
      if (m >= -1 && m != ifpan_kenwood_mode) {
        ifpan_kenwood_mode = m;
        g_idle_add(ext_vfo_update, NULL);
        if (m >= 0) { g_idle_add(mode_apply, GINT_TO_POINTER(m)); }
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
      if (llabs(f - hold_freq) < 10) {
        hold_until = 0;
      } else if (g_get_monotonic_time() < hold_until) {
        usleep(ifpan_poll_ms * 1000);
        continue;
      } else {
        hold_until = 0;
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
    // How fast the display follows the Kenwood dial: poll interval =
    // ifpan.poll_ms x "VFO Encoder Divisor" (Radio menu), so 1 = 50 ms,
    // 4 = 200 ms. A pending piHPSDR-IF -> Kenwood request ends the wait at
    // once, so tuning with the mouse is never delayed.
    //
    int div = (vfo_encoder_divisor < 1) ? 1 : vfo_encoder_divisor;
    gint64 wait_end = g_get_monotonic_time() + (gint64)ifpan_poll_ms * div * 1000;
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

static gboolean win_moved(GtkWidget *widget, GdkEvent *event, gpointer data) {
  int x, y;
  if (!win_tracking) { return FALSE; }
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  if ((x != 0 || y != 0) && (x != win_x || y != win_y)) {
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
  } else if (win_have_saved && pass == 1 && (x != win_x || y != win_y)) {
    //
    // Some window managers place the frame, others the client area, so the
    // position read back may differ from the one requested by the frame
    // size: compensate once.
    //
    gtk_window_move(GTK_WINDOW(top_window), 2 * win_x - x, 2 * win_y - y);
  }
  if (pass < 2) {
    g_timeout_add(1000, win_apply, GINT_TO_POINTER(pass + 1));
  } else {
    // from now on, only moves by the user are saved
    t_print("%s: window at %d,%d (saved %d,%d)\n", __func__, x, y, win_x, win_y);
    win_x = x;
    win_y = y;
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
      win_have_saved = 1;
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
  g_timeout_add(50, ui_watch, NULL);
  win_restore();
}
