/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
 * RADE mono streaming resampler (speexdsp, quality 5, proper low-pass).
 */
#ifndef RADE_RESAMPLE_H
#define RADE_RESAMPLE_H

typedef struct rade_rs rade_rs_t;

/* Returns NULL on failure.  Identical rates give a plain copy. */
rade_rs_t *rade_rs_new(int in_rate, int out_rate);
/* Streaming: returns samples written to out (<= max_out).  Input that does
 * not fit into max_out is dropped, so size out for n_in * out/in + a few. */
int rade_rs_process(rade_rs_t *rs, const float *in, int n_in, float *out, int max_out);
void rade_rs_free(rade_rs_t *rs);

#endif
