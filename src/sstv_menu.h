/* SSTV window for piHPSDR-IF
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _SSTV_MENU_H
#define _SSTV_MENU_H

#ifdef SSTV
#include <gtk/gtk.h>
extern void sstv_menu(GtkWidget *parent);                 // main menu "SSTV": dock / undock the panel
extern int  sstv_panel_wanted_width(int width, int height); // radio_reconfigure: width for the panel, 0: none
extern void sstv_panel_place(int x, int y, int w, int h);   // radio_reconfigure: put / size the panel
extern void sstv_menu_shutdown(void);
extern int  sstv_menu_is_open(void);                       // program exit: settings from before SSTV back
// SSTV tuning reference on RX1's panadapter and waterfall (hpsdr-rs): sync
// 1200 Hz and white 2300 Hz (purple), black 1500 Hz (blue), while the panel is open
extern void sstv_marks_draw(cairo_t *cr, double rxpos, double cAp, int lsb, double height);
#endif

#endif
