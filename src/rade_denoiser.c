/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <stdlib.h>
#include <string.h>
#include "rnnoise.h"
#include "rade_denoiser.h"

#define RD_FRAME_MAX 480

struct rade_denoiser {
  DenoiseState *st;
  int frame;
  float *pending;  /* input awaiting a full frame (int16 range) */
  int n_pending;
  float *outbuf;   /* denoised, not yet returned */
  int n_out;
  int cap;
};

rade_denoiser_t *rade_denoiser_new(void) {
  rade_denoiser_t *d = calloc(1, sizeof(*d));
  if (!d) { return NULL; }
  d->frame = rnnoise_get_frame_size();
  if (d->frame <= 0 || d->frame > RD_FRAME_MAX) {
    free(d);
    return NULL;
  }
  d->cap = d->frame * 64;
  d->pending = calloc((size_t)d->cap, sizeof(float));
  d->outbuf = calloc((size_t)d->cap, sizeof(float));
  d->st = rnnoise_create(NULL);
  if (!d->pending || !d->outbuf || !d->st) {
    rade_denoiser_free(d);
    return NULL;
  }
  return d;
}

void rade_denoiser_free(rade_denoiser_t *d) {
  if (!d) { return; }
  if (d->st) { rnnoise_destroy(d->st); }
  free(d->pending);
  free(d->outbuf);
  free(d);
}

void rade_denoiser_process(rade_denoiser_t *d, float *block, int n) {
  if (!d || !block || n <= 0) { return; }
  float frame_out[RD_FRAME_MAX];
  for (int i = 0; i < n; i++) {
    if (d->n_pending < d->cap) { d->pending[d->n_pending++] = block[i] * 32768.0f; }
    if (d->n_pending >= d->frame) {
      rnnoise_process_frame(d->st, frame_out, d->pending);
      d->n_pending -= d->frame;
      memmove(d->pending, d->pending + d->frame, (size_t)d->n_pending * sizeof(float));
      if (d->n_out + d->frame <= d->cap) {
        memcpy(d->outbuf + d->n_out, frame_out, (size_t)d->frame * sizeof(float));
        d->n_out += d->frame;
      }
    }
  }
  int m = n < d->n_out ? n : d->n_out;
  for (int i = 0; i < m; i++) {
    float v = d->outbuf[i] / 32768.0f;
    block[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
  }
  for (int i = m; i < n; i++) { block[i] = 0.0f; }
  d->n_out -= m;
  memmove(d->outbuf, d->outbuf + m, (size_t)d->n_out * sizeof(float));
}
