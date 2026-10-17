/* Copyright (C)
* 2026 - CU2ED (piHPSDR-IF)
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
// piHPSDR-IF: the RX audio as heard, recorded to a WAV file (as hpsdr-rs's
// "Record", audio_recorder.rs): 48 kHz, stereo, 16-bit PCM, streamed to disk
// (no buffer that grows with the length). Where the action is pressed: on
// the server (or a stand-alone radio) the active receiver's audio, on a
// client the RX audio it receives from the server. Files go to
// <settings folder>/recordings/piHPSDR-IF_YYYYMMDD_HHMMSS.wav; the header's
// sizes are filled in when the recording stops (also on program exit).
//

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "message.h"
#include "wav_rec.h"

#define WAV_RATE     48000
#define WAV_CHANNELS 2
#define WAV_BITS     16
#define WAV_MAX_DATA 0xFFFF0000u   // WAV sizes are 32 bit: stop well before

static GMutex   wav_mutex;
static FILE    *wav_file = NULL;
static uint32_t wav_bytes = 0;
static gint64   wav_start_us = 0;
static char     wav_name[512] = "";

static void put16(unsigned char *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(unsigned char *p, uint32_t v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static void write_header(FILE *f, uint32_t data_bytes) {
  unsigned char h[44];
  memcpy(h, "RIFF", 4);
  put32(h + 4, 36u + data_bytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);                                        // fmt chunk size (PCM)
  put16(h + 20, 1);                                         // PCM
  put16(h + 22, WAV_CHANNELS);
  put32(h + 24, WAV_RATE);
  put32(h + 28, WAV_RATE * WAV_CHANNELS * (WAV_BITS / 8));  // byte rate
  put16(h + 32, WAV_CHANNELS * (WAV_BITS / 8));             // block align
  put16(h + 34, WAV_BITS);
  memcpy(h + 36, "data", 4);
  put32(h + 40, data_bytes);
  fseek(f, 0, SEEK_SET);
  fwrite(h, 1, sizeof(h), f);
}

// the folder the props file is in: the working directory on Linux, the
// settings folder on Windows (the program changes to it at start-up)
static void recordings_path(char *buf, size_t len) {
  char *cwd = g_get_current_dir();
  char *dir = g_build_filename(cwd, "recordings", NULL);
  g_mkdir_with_parents(dir, 0755);
  time_t now = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  tmv = *localtime(&now);       // called from the GTK thread only
#else
  localtime_r(&now, &tmv);
#endif
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
  char fname[64];
  snprintf(fname, sizeof(fname), "piHPSDR-IF_%s.wav", stamp);
  char *full = g_build_filename(dir, fname, NULL);
  snprintf(buf, len, "%s", full);
  g_free(full);
  g_free(dir);
  g_free(cwd);
}

int wav_rec_active(void) {
  return wav_file != NULL;
}

int wav_rec_seconds(void) {
  if (wav_file == NULL) { return 0; }
  return (int)((g_get_monotonic_time() - wav_start_us) / 1000000);
}

void wav_rec_start(void) {
  g_mutex_lock(&wav_mutex);
  if (wav_file == NULL) {
    recordings_path(wav_name, sizeof(wav_name));
    wav_file = g_fopen(wav_name, "wb");
    if (wav_file == NULL) {
      t_print("%s: cannot create %s\n", __func__, wav_name);
    } else {
      wav_bytes = 0;
      write_header(wav_file, 0);   // sizes filled in by wav_rec_stop
      wav_start_us = g_get_monotonic_time();
      t_print("%s: recording the RX audio to %s\n", __func__, wav_name);
    }
  }
  g_mutex_unlock(&wav_mutex);
}

void wav_rec_stop(void) {
  g_mutex_lock(&wav_mutex);
  if (wav_file != NULL) {
    write_header(wav_file, wav_bytes);
    fclose(wav_file);
    wav_file = NULL;
    t_print("%s: %s closed (%u s)\n", __func__, wav_name, (unsigned)(wav_bytes / (WAV_RATE * 4)));
  }
  g_mutex_unlock(&wav_mutex);
}

void wav_rec_toggle(void) {
  if (wav_rec_active()) { wav_rec_stop(); } else { wav_rec_start(); }
}

void wav_rec_frame(double left, double right) {
  if (wav_file == NULL) { return; }          // cheap when not recording
  g_mutex_lock(&wav_mutex);
  if (wav_file != NULL) {
    if (left > 1.0) { left = 1.0; } else if (left < -1.0) { left = -1.0; }
    if (right > 1.0) { right = 1.0; } else if (right < -1.0) { right = -1.0; }
    int16_t l = (int16_t)(left * 32767.0);
    int16_t r = (int16_t)(right * 32767.0);
    unsigned char b[4];
    put16(b, (uint16_t) l);
    put16(b + 2, (uint16_t) r);
    if (fwrite(b, 1, 4, wav_file) == 4) { wav_bytes += 4; }
    if (wav_bytes >= WAV_MAX_DATA) {
      g_mutex_unlock(&wav_mutex);
      wav_rec_stop();
      return;
    }
  }
  g_mutex_unlock(&wav_mutex);
}
