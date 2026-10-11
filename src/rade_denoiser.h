/* Copyright (C)
* 2024-2026 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
 * RADE mic denoiser: RNNoise wrapper (48 kHz, 480-sample frames).
 * Output is delayed by up to one frame; zeros until samples are available.
 */
#ifndef RADE_DENOISER_H
#define RADE_DENOISER_H

typedef struct rade_denoiser rade_denoiser_t;

rade_denoiser_t *rade_denoiser_new(void);
void rade_denoiser_free(rade_denoiser_t *d);
/* In place, full-scale normalised floats (+/-1.0), any block size. */
void rade_denoiser_process(rade_denoiser_t *d, float *block, int n);

#endif
