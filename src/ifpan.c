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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "ext.h"
#include "ifpan.h"
#include "main.h"
#include "message.h"
#include "property.h"
#include "radio.h"
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
static long long ui_last_freq = -1;   // receiver frequency last seen (main thread only)

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
  if (ifpan_poll_ms < 20) { ifpan_poll_ms = 20; }
  // keep the IF well inside the Pluto span (+/- 384 kHz at 768 kHz)
  if (ifpan_lo_offset >  200000) { ifpan_lo_offset =  200000; }
  if (ifpan_lo_offset < -200000) { ifpan_lo_offset = -200000; }
}

static gboolean cat_apply(gpointer data) {
  long long f;
  g_mutex_lock(&cat_mutex);
  f = cat_pending;
  cat_scheduled = 0;
  g_mutex_unlock(&cat_mutex);
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
  return G_SOURCE_CONTINUE;
}

static int cat_connect(void) {
  struct addrinfo hints, *res, *ai;
  char port[16];
  int sock = -1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d", ifpan_rigctld_port);
  if (getaddrinfo(ifpan_rigctld_host, port, &hints, &res) != 0) { return -1; }
  for (ai = res; ai != NULL; ai = ai->ai_next) {
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) { continue; }
    if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) { break; }
    close(sock);
    sock = -1;
  }
  freeaddrinfo(res);
  if (sock >= 0) {
    // long enough for rigctld's own retries when the radio does not answer
    struct timeval tv = { 6, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  return sock;
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

static gpointer cat_thread(gpointer data) {
  int sock = -1;
  long long hold_freq = 0;            // frequency just sent to the Kenwood
  gint64 hold_until = 0;              // until then, ignore other readings
  int trouble = 0;                    // a CAT problem has been reported (log once)
  int mode_count = 0;                 // polls until the next mode read (0: now)
  int smeter_count = 0;               // polls until the next S-meter read
  t_print("%s: rigctld at %s:%d, IF=%lld Hz\n", __func__, ifpan_rigctld_host, ifpan_rigctld_port, ifpan_if_freq);
  for (;;) {
    if (sock < 0) {
      sock = cat_connect();
      if (sock < 0) {
        usleep(1000000);
        continue;
      }
      if (cat_set_cache(sock, ifpan_rigctld_cache_ms) < 0) {
        // rigctld accepted the connection but is stuck opening the radio
        if (!trouble) {
          t_print("%s: rigctld not answering (radio off, wrong baud rate or cable?)\n", __func__);
          trouble = 1;
          ifpan_cat_connected = 0;
          g_idle_add(ext_vfo_update, NULL);
        }
        close(sock);
        sock = -1;
        usleep(1000000);
        continue;
      }
      t_print("%s: connected to rigctld\n", __func__);
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
      if (cat_set_freq(sock, target) < 0) {
        t_print("%s: rigctld connection lost\n", __func__);
        ifpan_cat_connected = 0;
        g_idle_add(ext_vfo_update, NULL);
        close(sock);
        sock = -1;
        continue;
      }
      hold_freq = target;
      hold_until = g_get_monotonic_time() + CAT_SET_HOLD_US;
    }
    long long f = cat_get_freq(sock);
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
        t_print("%s: rigctld connection lost\n", __func__);
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
      int m = cat_get_mode(sock);
      if (m == -1) {
        close(sock);
        sock = -1;
        continue;
      }
      if (m == -3) { m = -1; }
      if (m >= -1 && m != ifpan_kenwood_mode) {
        ifpan_kenwood_mode = m;
        g_idle_add(ext_vfo_update, NULL);
      }
    }
    //
    // Kenwood S-meter, shown next to the piHPSDR-IF meter (the meter is
    // redrawn continuously, so only the value is stored here)
    //
    if (smeter_count-- <= 0) {
      smeter_count = CAT_SMETER_EVERY;
      int db;
      int rc = cat_get_strength(sock, &db);
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
      g_mutex_lock(&cat_mutex);
      pending = cat_set_target;
      g_mutex_unlock(&cat_mutex);
      if (pending > 0) { break; }
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
  cat_thread_id = g_thread_new("ifpan_cat", cat_thread, NULL);
  g_timeout_add(50, ui_watch, NULL);
  win_restore();
}
