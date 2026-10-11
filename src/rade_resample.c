/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <stdlib.h>
#include <string.h>
#include <speex/speex_resampler.h>
#include "rade_resample.h"

struct rade_rs {
  SpeexResamplerState *st;
  int identity;
};

rade_rs_t *rade_rs_new(int in_rate, int out_rate) {
  if (in_rate <= 0 || out_rate <= 0) { return NULL; }
  rade_rs_t *rs = calloc(1, sizeof(*rs));
  if (!rs) { return NULL; }
  if (in_rate == out_rate) {
    rs->identity = 1;
    return rs;
  }
  int err = 0;
  rs->st = speex_resampler_init(1, in_rate, out_rate, 5, &err);
  if (!rs->st || err != 0) {
    free(rs);
    return NULL;
  }
  return rs;
}

int rade_rs_process(rade_rs_t *rs, const float *in, int n_in, float *out, int max_out) {
  if (!rs || !in || !out || n_in <= 0 || max_out <= 0) { return 0; }
  if (rs->identity) {
    int n = n_in < max_out ? n_in : max_out;
    memcpy(out, in, (size_t)n * sizeof(float));
    return n;
  }
  int total = 0;
  while (n_in > 0 && total < max_out) {
    spx_uint32_t ilen = (spx_uint32_t)n_in;
    spx_uint32_t olen = (spx_uint32_t)(max_out - total);
    if (speex_resampler_process_float(rs->st, 0, in, &ilen, out + total, &olen) != 0) { break; }
    in += ilen;
    n_in -= (int)ilen;
    total += (int)olen;
    if (ilen == 0 && olen == 0) { break; }
  }
  return total;
}

void rade_rs_free(rade_rs_t *rs) {
  if (!rs) { return; }
  if (rs->st) { speex_resampler_destroy(rs->st); }
  free(rs);
}
