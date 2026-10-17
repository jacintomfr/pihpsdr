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

#ifndef _WAV_REC_H_
#define _WAV_REC_H_

// RX audio as heard -> WAV file (48 kHz stereo 16-bit), see wav_rec.c
extern void wav_rec_start(void);
extern void wav_rec_stop(void);
extern void wav_rec_toggle(void);
extern int  wav_rec_active(void);
extern int  wav_rec_seconds(void);
extern void wav_rec_frame(double left, double right);   // one frame, any thread

#endif
