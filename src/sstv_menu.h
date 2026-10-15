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
#endif

#endif
