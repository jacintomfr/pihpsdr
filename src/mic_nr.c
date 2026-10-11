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
// Microphone noise reduction with RNNoise (xiph, BSD-3-Clause, sources in
// ../rnnoise), the same library, model and conventions as the user's
// hpsdr-rs (rade_denoiser.rs), so that both can be compared:
//  - fixed frames of 480 samples (10 ms at its native 48 kHz),
//  - int16-range floats (+/-32768) in and out,
//  - the built-in model (rnnoise_create(NULL)).
// One sample in, one sample out: a whole frame is processed every 480
// samples, so the delay is exactly 10 ms.
//

#include <glib.h>

#include "mic_nr.h"
#include "rnnoise.h"

struct _mic_nr {
  DenoiseState *st;
  int    frame;                       // RNNoise frame size (480)
  int    pos;                         // position in the current frame
  float  in[512];                     // frame being collected (int16 range)
  float  out[512];                    // previous frame, denoised (int16 range)
};

MIC_NR *mic_nr_create(void) {
  MIC_NR *m = g_new0(MIC_NR, 1);
  m->st = rnnoise_create(NULL);
  m->frame = rnnoise_get_frame_size();
  if (m->frame > 512) { m->frame = 512; }   // never: RNNoise uses 480
  return m;
}

void mic_nr_destroy(MIC_NR *m) {
  if (m == NULL) { return; }
  rnnoise_destroy(m->st);
  g_free(m);
}

double mic_nr_process(MIC_NR *m, double sample) {
  double y = m->out[m->pos] / 32768.0;
  m->in[m->pos] = (float)(sample * 32768.0);
  if (++m->pos >= m->frame) {
    rnnoise_process_frame(m->st, m->out, m->in);
    m->pos = 0;
  }
  if (y > 1.0) { y = 1.0; }
  if (y < -1.0) { y = -1.0; }
  return y;
}
