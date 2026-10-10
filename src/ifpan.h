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
// IF panadapter mode (compile-time option IFPAN):
//
// The PlutoSDR is tapped into the 1st IF of a Kenwood TS-570D and stays
// tuned to that IF for ever. The Kenwood's VFO frequency is read via CAT
// (Hamlib rigctld) and becomes the centre frequency of the display, so
// that every point of the spectrum is shown at its real RF frequency:
//
//   displayed freq = Kenwood VFO + (SDR offset from the IF centre)
//
// piHPSDR runs strictly receive-only in this mode (no transmitter object
// is ever created and all Soapy TX entry points are no-ops).
//

#ifndef _IFPAN_H_
#define _IFPAN_H_

#define IFPAN_DEFAULT_IF_FREQ  73050000LL   // TS-570D 1st IF

extern long long ifpan_if_freq;       // IF centre (= Kenwood VFO) in Hz
extern long long ifpan_lo_offset;     // Pluto LO = IF + this, keeps the DC spike off the VFO
extern char      ifpan_rigctld_host[64];
extern int       ifpan_rigctld_port;
extern int       ifpan_poll_ms;
extern int       ifpan_cat_connected; // 1 while rigctld answers
extern int       ifpan_cat_write;     // 1: tuning in piHPSDR-IF also tunes the Kenwood
extern int       ifpan_follow_vfo;    // 1: the receiver follows the Kenwood dial

extern void      ifpan_set_follow(int on);

extern long long ifpan_hw_frequency(void);
extern void      ifpan_start(void);
extern void      ifpan_save_state(void);
extern void      ifpan_restore_state(void);

#endif
