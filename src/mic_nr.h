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
// Microphone noise reduction with RNNoise (as in hpsdr-rs), applied to the
// raw 48 kHz microphone where it is captured - on a remote client, before
// VOX and before the audio is sent - so that VOX and the whole TX chain
// see the cleaned signal. TX menu, DEXP page ("Mic Noise Reduction").
//

#ifndef _MIC_NR_H_
#define _MIC_NR_H_

typedef struct _mic_nr MIC_NR;

extern MIC_NR *mic_nr_create(void);
extern void    mic_nr_destroy(MIC_NR *m);
extern double  mic_nr_process(MIC_NR *m, double sample);   // one 48 kHz sample in, one out

#endif
