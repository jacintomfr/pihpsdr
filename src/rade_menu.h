/* Copyright (C)
* RADE V1 menu for deskHPSDR.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef _RADE_MENU_H
#define _RADE_MENU_H

#ifdef RADE
#include <gtk/gtk.h>
extern void rade_menu(GtkWidget *parent);                 // main menu "RADE": dock / undock the panel
extern int  rade_menu_is_open(void);
extern int  rade_panel_wanted_width(int width, int height); // radio_reconfigure: width for the panel, 0: none
extern void rade_panel_place(int x, int y, int w, int h);   // radio_reconfigure: put / size the panel
extern void rade_menu_shutdown(void);                       // program exit: zoom / pan from before RADE back
#endif

#endif
