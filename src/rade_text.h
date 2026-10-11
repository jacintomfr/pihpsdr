/* Copyright (C)
* RADE V1 End-of-Over callsign codec (C port).
*
* Ported from the Rust port in hpsdr-rs (adapted from SDRoxide, GPL-3.0-or-later),
* itself a port of rade_text.cpp / ldpc_encode.cpp / ldpc_decode.cpp from
* https://github.com/tmiw/freedv-backend (BSD-2-Clause, Mooneer Salem).
*
* SPDX-License-Identifier: GPL-3.0-or-later
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

#ifndef _RADE_TEXT_H
#define _RADE_TEXT_H

#define RADE_TEXT_EOO_FLOATS 180
#define RADE_TEXT_MAX_CHARS  8

/*
 * Fill eoo_out with the End-of-Over floats (90 I/Q symbols) carrying callsign.
 * Characters outside the 6-bit alphabet are dropped, lowercase is upper-cased,
 * only the first 8 characters are considered.
 */
void rade_text_encode(const char *callsign, float eoo_out[180]);

/*
 * Recover a callsign from an End-of-Over frame.  Returns 1 and fills call_out
 * (NUL terminated, up to 8 characters) on success; returns 0 if the LDPC
 * decoder did not converge, the CRC mismatches or no text was carried.
 */
int rade_text_decode(const float eoo_in[180], char call_out[9]);

#endif
