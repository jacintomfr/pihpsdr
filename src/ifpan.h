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
extern int       ifpan_kenwood_mode;  // index into ifpan_mode_names[], -1: unknown
extern int       ifpan_kenwood_smeter; // S-meter in dB relative to S9, -999: unknown
extern const char *ifpan_mode_names[];
extern int       ifpan_tx_mute;       // 1: mute the RX audio while the Kenwood transmits
extern int       ifpan_follow_mode;   // 1: the receiver mode follows the Kenwood mode
extern int       ifpan_kenwood_tx;    // 1: the Kenwood is transmitting (Kenwood TCP only)
extern int       ifpan_kenwood_pwr;   // raw TX meters, -1: not read yet
extern int       ifpan_kenwood_swr;
extern int       ifpan_kenwood_alc;

extern void      ifpan_set_follow(int on);
extern void      ifpan_set_follow_mode(int on);

#define IFPAN_CAT_RIGCTLD 0           // Hamlib rigctld (TCP, rigctl protocol)
#define IFPAN_CAT_KENWOOD 1           // Kenwood CAT over TCP (RS-232/WiFi bridge)
extern int       ifpan_cat_backend;
extern char      ifpan_kenwood_host[64];
extern int       ifpan_kenwood_port;
extern void      ifpan_cat_reconnect(void);   // apply new CAT settings now

extern long long ifpan_hw_frequency(void);
extern void      ifpan_start(void);
extern void      ifpan_stop(void);    // on exit: Kenwood back to RX if we keyed it
extern void      ifpan_ptt(int on);   // toolbar PTT / MOX: 1 TX, 0 RX, -1 toggle
extern void      ifpan_ts_here(void); // toolbar "TS here": Kenwood to the RX frequency
extern void      ifpan_band_changed(long long f);   // band menu: Kenwood to the new band

//
// TS-570 frequency coverage: the band menu and frequency checks use this
// instead of the SDR's own range (the SDR stays on the IF)
//
#define IFPAN_RADIO_FMIN     100000LL
#define IFPAN_RADIO_FMAX   60000000LL
extern void      ifpan_save_state(void);
extern void      ifpan_restore_state(void);

#endif
