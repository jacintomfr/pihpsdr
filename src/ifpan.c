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
//   rigctld -m 2016 -r /dev/ttyUSB0 -s 9600
//
// A thread polls the VFO frequency ("f") and, when it changes, hands it
// to the GTK main loop which moves the display centre. The PlutoSDR LO
// is never touched, so the RX stream is not interrupted while the
// Kenwood dial is turned.
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

static GThread *cat_thread_id = NULL;
static GMutex   cat_mutex;
static long long cat_pending = 0;     // last frequency read, waiting for the main loop
static int       cat_scheduled = 0;   // an idle callback is already queued

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
}

void ifpan_restore_state(void) {
  GetPropI0("ifpan.if_freq",       ifpan_if_freq);
  GetPropS0("ifpan.rigctld_host",  ifpan_rigctld_host);
  GetPropI0("ifpan.rigctld_port",  ifpan_rigctld_port);
  GetPropI0("ifpan.poll_ms",       ifpan_poll_ms);
  GetPropI0("ifpan.lo_offset",     ifpan_lo_offset);
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
  return G_SOURCE_REMOVE;
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
    struct timeval tv = { 0, 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  return sock;
}

//
// Send "f\n" and read one line. Returns the frequency, or -1 on error.
//
static long long cat_get_freq(int sock) {
  char line[64];
  size_t len = 0;
  if (send(sock, "f\n", 2, MSG_NOSIGNAL) != 2) { return -1; }
  while (len < sizeof(line) - 1) {
    ssize_t n = recv(sock, &line[len], 1, 0);
    if (n <= 0) { return -1; }
    if (line[len] == '\n') { break; }
    len++;
  }
  line[len] = 0;
  if (strncmp(line, "RPRT", 4) == 0) { return -1; }   // rigctld error report
  char *end;
  long long f = strtoll(line, &end, 10);
  if (end == line || f <= 0) { return -1; }
  return f;
}

static gpointer cat_thread(gpointer data) {
  int sock = -1;
  t_print("%s: rigctld at %s:%d, IF=%lld Hz\n", __func__, ifpan_rigctld_host, ifpan_rigctld_port, ifpan_if_freq);
  for (;;) {
    if (sock < 0) {
      sock = cat_connect();
      if (sock < 0) {
        usleep(1000000);
        continue;
      }
      t_print("%s: connected to rigctld\n", __func__);
      ifpan_cat_connected = 1;
      g_idle_add(ext_vfo_update, NULL);
    }
    long long f = cat_get_freq(sock);
    if (f < 0) {
      t_print("%s: rigctld connection lost\n", __func__);
      ifpan_cat_connected = 0;
      g_idle_add(ext_vfo_update, NULL);
      close(sock);
      sock = -1;
      continue;
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
    usleep(ifpan_poll_ms * 1000);
  }
  return NULL;
}

void ifpan_start(void) {
  if (cat_thread_id != NULL) { return; }
  cat_thread_id = g_thread_new("ifpan_cat", cat_thread, NULL);
}
