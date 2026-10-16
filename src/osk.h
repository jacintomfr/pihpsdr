/* On-screen keyboard for piHPSDR-IF (touch screen), see osk.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _OSK_H
#define _OSK_H

#ifdef IFPAN
extern void osk_init(void);           // once, at start: every text field gets the keyboard on focus
extern void osk_set_enabled(int on);
extern int  osk_get_enabled(void);
extern int  touch_style;              // rounded dark buttons, X check boxes (ifpan.touch_style)
extern void touch_style_set(int on);  // apply now (and on start, after the settings are read)
#endif

#endif
