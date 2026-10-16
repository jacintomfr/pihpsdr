/* Copyright (C)
* RADE V1 panel for piHPSDR-IF (UI like the hpsdr-rs RADE panel), ported from
* the deskHPSDR Windows RADE port. Differences: own callsign edited here
* (piHPSDR has no "own callsign"), no FreeDV Reporter yet, Quick Tune moves
* the Kenwood (band change by CAT), settings saved when the panel closes. On
* a client the panel acts on the server's RADE engine and shows its state
* (rade_glue, INFO_RADE / CMD_RADE).
*
* Docked like the SSTV panel: the screen split in half as piHPSDR's two
* receivers side by side, spectrum / waterfall on the left (zoom 26 centred
* on the RADE signal), the RADE controls on the right; the EQ sliders and the
* Quick Tune bands open in pop-ups so that everything fits 1024x600.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifdef RADE

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "new_menu.h"
#include "filter.h"
#include "mode.h"
#include "radio.h"
#include "receiver.h"
#include "vfo.h"
#include "ext.h"
#include "rade_glue.h"
#include "rade_menu.h"
#ifdef SSTV
  #include "sstv_menu.h"
#endif
#ifdef IFPAN
  #include "ifpan.h"
#endif

static GtkWidget *panel = NULL;      // docked panel (in the main window's GtkFixed)
static int panel_x = -1, panel_y = -1;
static guint timer_id = 0;
static int updating = 0;
static int saved_zoom = -1;          // RX1 zoom / pan before the panel opened (zoom 26 while it is open)
static int saved_pan = 0;

static GtkWidget *rx_check, *tx_check, *status_label, *level_bar, *calls_label, *fit_label;
static GtkWidget *opt_check[6];   // denoiser leveler compressor EQ mute squelch
static GtkWidget *eq_scale[4];

#define RADE_CALLS_SHOWN 3        // received callsigns listed in the panel

//
// The RADE signal (DIGU 700 - 2300 Hz, centred at 1500 Hz audio) in the
// middle of RX1's panadapter, as the SSTV panel does with its band; the pan
// steps are coarse (~460 Hz at 96 kHz)
//
static void center_pan(void) {
  if (receivers < 1 || receiver[0] == NULL || receiver[0]->zoom <= 1) { return; }
  const RECEIVER *rx = receiver[0];
  int m = vfo[0].mode;
  double sign = (m == modeLSB || m == modeDIGL) ? -1.0 : 1.0;
  long long f = vfo[0].ctun ? vfo[0].ctun_frequency : vfo[0].frequency;
  double offset = (double)(f - vfo[0].frequency) + sign * 1500.0 + rx->sample_rate / 2.0;
  double z = (offset * rx->zoom / (double)rx->sample_rate - 0.5) / (double)(rx->zoom - 1);
  radio_set_pan(0, (int)lround(200.0 * z) - 100);
}

static gboolean center_pan_cb(gpointer data) {
  (void)data;
  if (panel != NULL) { center_pan(); }
  return G_SOURCE_REMOVE;
}

static void small_font(GtkWidget *w) {
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_data(css, "label { font-size: 13px; }", -1, NULL);
  gtk_style_context_add_provider(gtk_widget_get_style_context(w), GTK_STYLE_PROVIDER(css),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
}

//
// Closing the panel leaves RADE: band, frequency, mode, filter, volume and TX
// Drive go back to before RADE (rade_glue snapshot), the zoom / pan too.
//
static void panel_close(void) {
  if (timer_id) { g_source_remove(timer_id); timer_id = 0; }
  if (panel == NULL) { return; }
  if (rade_glue_get_rx_enabled() || rade_glue_get_tx_armed()) { rade_glue_set_rx_enabled(0); }
  GtkWidget *tmp = panel;
  panel = NULL;
  gtk_widget_destroy(tmp);
  g_object_unref(tmp);        // the reference taken in rade_menu
  if (saved_zoom > 0) {
    radio_set_zoom(0, saved_zoom);
    radio_set_pan(0, saved_pan);
    saved_zoom = -1;
  }
  panel_x = panel_y = -1;
  radio_reconfigure();        // the spectrum / waterfall get the whole width back
  radio_save_state();
}

static void set_check(GtkWidget *w, int on) {
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) != (on != 0)) {
    updating = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), on != 0);
    updating = 0;
  }
}

static void sync_checks(void) {
  set_check(rx_check, rade_glue_get_rx_enabled());
  set_check(tx_check, rade_glue_get_tx_armed());
}

static void rx_cb(GtkWidget *widget, gpointer data) {
  (void)data;
  if (updating) { return; }
  rade_glue_set_rx_enabled(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  sync_checks();
}

static void tx_cb(GtkWidget *widget, gpointer data) {
  (void)data;
  if (updating) { return; }
  rade_glue_set_tx_armed(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  sync_checks();
}

#define OPT_CB(fn, setter) \
  static void fn(GtkWidget *widget, gpointer data) { \
    (void)data; \
    if (updating) { return; } \
    setter(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))); \
  }
OPT_CB(denoiser_cb, rade_glue_set_denoiser)
OPT_CB(leveler_cb, rade_glue_set_leveler)
OPT_CB(compressor_cb, rade_glue_set_compressor)
OPT_CB(eq_cb, rade_glue_set_eq)
OPT_CB(mute_cb, rade_glue_set_mute_analog)
OPT_CB(squelch_cb, rade_glue_set_squelch)

static void callsign_cb(GtkWidget *widget, gpointer data) {
  (void)data;
  rade_glue_set_callsign(gtk_entry_get_text(GTK_ENTRY(widget)));
}

static void eq_gain_cb(GtkRange *range, gpointer data) {
  if (updating) { return; }
  rade_glue_set_eq_gain(GPOINTER_TO_INT(data), (float) gtk_range_get_value(range));
}

static void update_fit_label(void) {
  gtk_label_set_text(GTK_LABEL(fit_label), rade_glue_get_filter_wide()
                     ? "RX filter: 300-2700 Hz (wide)" : "RX filter: 700-2300 Hz (narrow)");
}

// the Var1 filter is set by rade_glue (on the server, also from a client)
static void fit_cb(GtkWidget *widget, gpointer data) {
  (void)widget;
  (void)data;
  int mode = vfo[active_receiver->id].mode;
  if (mode != modeDIGU && mode != modeDIGL) {
    gtk_label_set_text(GTK_LABEL(fit_label), "Fit Filter needs DIGU/DIGL mode");
    return;
  }
  rade_glue_fit_filter();
  update_fit_label();
}

static void quick_tune_cb(GtkWidget *widget, gpointer data) {
  (void)widget;
  rade_glue_quick_tune(GPOINTER_TO_INT(data));
  g_timeout_add(300, center_pan_cb, NULL);   // once the new frequency / mode is in place
}

//
// On a client the settings may also change on the server (its own panel):
// the check boxes and sliders follow the state the server sends.
//
static void sync_options(void) {
  static int (*const get[6])(void) = { rade_glue_get_denoiser, rade_glue_get_leveler, rade_glue_get_compressor,
                                       rade_glue_get_eq, rade_glue_get_mute_analog, rade_glue_get_squelch
                                     };
  for (int i = 0; i < 6; i++) { set_check(opt_check[i], get[i]()); }
  for (int i = 0; i < 4; i++) {
    if (fabs(gtk_range_get_value(GTK_RANGE(eq_scale[i])) - rade_glue_get_eq_gain(i)) > 0.01) {
      updating = 1;
      gtk_range_set_value(GTK_RANGE(eq_scale[i]), rade_glue_get_eq_gain(i));
      updating = 0;
    }
  }
  update_fit_label();
}

static gboolean status_timer(gpointer data) {
  (void)data;
  if (panel == NULL) { timer_id = 0; return G_SOURCE_REMOVE; }
  sync_checks();
  if (radio_is_remote) { sync_options(); }
  char buf[160];
  int vm = vfo[active_receiver->id].mode;
  if (rade_glue_get_rx_enabled() && vm != modeDIGU && vm != modeDIGL) {
    gtk_label_set_text(GTK_LABEL(status_label), "!! RADE needs mode DIGU/DIGL (use Quick Tune or change the mode) !!");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(level_bar), 0.0);
  } else if (!rade_glue_engine_running()) {
    gtk_label_set_text(GTK_LABEL(status_label), "RADE engine stopped");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(level_bar), 0.0);
  } else {
    rade_engine_stats_t st;
    rade_glue_get_stats(&st);
    if (st.sync) {
      snprintf(buf, sizeof(buf), "SYNC   SNR %+.1f dB   Offset %+.1f Hz   EOO %llu   Dropped %llu",
               st.snr_db, st.foff_hz, (unsigned long long) st.eoo_count, (unsigned long long) st.dropped);
    } else {
      snprintf(buf, sizeof(buf), "no sync   EOO %llu   Dropped %llu",
               (unsigned long long) st.eoo_count, (unsigned long long) st.dropped);
    }
    gtk_label_set_text(GTK_LABEL(status_label), buf);
    double lv = st.level;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(level_bar), lv < 0 ? 0 : (lv > 1 ? 1 : lv));
  }
  char list[RADE_CALLS_SHOWN * 40];
  list[0] = 0;
  int n = rade_glue_callsign_count();
  int shown = 0;
  for (int i = 0; i < n && shown < RADE_CALLS_SHOWN; i++) {
    char call[9];
    float snr;
    if (rade_glue_get_callsign(i, call, &snr)) {
      char line[40];
      snprintf(line, sizeof(line), "%s%-8s  %+.1f dB", shown ? "\n" : "", call, snr);
      strncat(list, line, sizeof(list) - strlen(list) - 1);
      shown++;
    }
  }
  gtk_label_set_text(GTK_LABEL(calls_label), shown ? list : "(none yet)");
  return G_SOURCE_CONTINUE;
}

//
// Touch size (1024x600 screen): 40 px rows, 24 px check boxes, a thicker level bar
//
#define ROW_H 40

static void touch_css(GtkWidget *w, const char *rule) {
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_data(css, rule, -1, NULL);
  gtk_style_context_add_provider(gtk_widget_get_style_context(w), GTK_STYLE_PROVIDER(css),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
}

static GtkWidget *add_check(const char *text, int on, GCallback cb) {
  GtkWidget *b = gtk_check_button_new_with_label(text);
  gtk_widget_set_size_request(b, -1, ROW_H);
  // a big square with an X (as the hpsdr-rs kiosk's check boxes), bigger text
  touch_css(b, "check { min-width: 28px; min-height: 28px; -gtk-icon-source: none; background-image: none;"
               " background-color: rgb(25, 25, 25); border: 2px solid rgb(170, 170, 170); border-radius: 3px; }"
               " check:checked { -gtk-icon-source: -gtk-icontheme('window-close-symbolic'); color: rgb(255, 255, 255); }"
               " label { font-size: 17px; }");
  gtk_widget_set_name(b, "boldlabel");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), on);
  g_signal_connect(b, "toggled", cb, NULL);
  return b;
}

static GtkWidget *new_label(const char *text, GtkAlign align) {
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_set_name(l, "boldlabel");
  gtk_widget_set_halign(l, align);
  return l;
}

// a button that opens `content` in a pop-up above it (closed by clicking outside)
static GtkWidget *popup_button(const char *text, GtkWidget *content, GtkWidget **pop) {
  GtkWidget *b = gtk_button_new_with_label(text);
  gtk_widget_set_size_request(b, 110, ROW_H);
  *pop = gtk_popover_new(b);
  gtk_popover_set_position(GTK_POPOVER(*pop), GTK_POS_TOP);
  gtk_container_set_border_width(GTK_CONTAINER(content), 8);
  gtk_widget_show_all(content);
  gtk_container_add(GTK_CONTAINER(*pop), content);
  g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popup), *pop);
  return b;
}

static GtkWidget *build_panel(void) {
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
  // (on a client, everything here acts on the server's RADE engine)
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_size_request(row, -1, ROW_H);
  rx_check = add_check("Enable RADE RX", rade_glue_get_rx_enabled(), G_CALLBACK(rx_cb));
  tx_check = add_check("TX armed", rade_glue_get_tx_armed(), G_CALLBACK(tx_cb));
  if (transmitter == NULL) { gtk_widget_set_sensitive(tx_check, FALSE); }
  gtk_box_pack_start(GTK_BOX(row), rx_check, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), tx_check, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
  status_label = new_label("", GTK_ALIGN_START);
  gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(status_label), 60);
  small_font(status_label);
  gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(row), new_label("Level", GTK_ALIGN_START), FALSE, FALSE, 0);
  level_bar = gtk_progress_bar_new();
  gtk_widget_set_valign(level_bar, GTK_ALIGN_CENTER);
  touch_css(level_bar, "trough, progress { min-height: 14px; }");
  gtk_box_pack_start(GTK_BOX(row), level_bar, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *rc = new_label("Received:", GTK_ALIGN_START);
  gtk_widget_set_valign(rc, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(row), rc, FALSE, FALSE, 0);
  calls_label = new_label("(none yet)", GTK_ALIGN_START);
  gtk_label_set_lines(GTK_LABEL(calls_label), RADE_CALLS_SHOWN);
  small_font(calls_label);
  gtk_box_pack_start(GTK_BOX(row), calls_label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(row), new_label("My callsign:", GTK_ALIGN_START), FALSE, FALSE, 0);
  GtkWidget *call_entry = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(call_entry), 8);
  gtk_entry_set_width_chars(GTK_ENTRY(call_entry), 9);
  gtk_widget_set_size_request(call_entry, -1, ROW_H);
  gtk_entry_set_text(GTK_ENTRY(call_entry), rade_glue_get_callsign_own());
  gtk_widget_set_tooltip_text(call_entry, "Sent in the End-of-Over frame of every RADE transmission");
  g_signal_connect(call_entry, "changed", G_CALLBACK(callsign_cb), NULL);
  gtk_box_pack_start(GTK_BOX(row), call_entry, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
  //
  // options: two columns (three did not fit half of a 1024 px screen); the EQ gains in a pop-up (they apply only while
  // the EQ check box is on): bass 100 Hz, mid 1500 Hz, treble 3000 Hz, volume
  //
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  opt_check[0] = add_check("Denoiser", rade_glue_get_denoiser(), G_CALLBACK(denoiser_cb));
  opt_check[1] = add_check("Leveler", rade_glue_get_leveler(), G_CALLBACK(leveler_cb));
  opt_check[2] = add_check("Compressor", rade_glue_get_compressor(), G_CALLBACK(compressor_cb));
  opt_check[3] = add_check("EQ", rade_glue_get_eq(), G_CALLBACK(eq_cb));
  opt_check[4] = add_check("Mute analog audio", rade_glue_get_mute_analog(), G_CALLBACK(mute_cb));
  opt_check[5] = add_check("Squelch gate", rade_glue_get_squelch(), G_CALLBACK(squelch_cb));
  gtk_grid_attach(GTK_GRID(grid), opt_check[0], 0, 0, 1, 1);   // Denoiser   | Leveler
  gtk_grid_attach(GTK_GRID(grid), opt_check[1], 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), opt_check[2], 0, 1, 1, 1);   // Compressor | Squelch gate
  gtk_grid_attach(GTK_GRID(grid), opt_check[5], 1, 1, 1, 1);
  GtkWidget *eq_grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(eq_grid), 10);
  gtk_grid_set_row_spacing(GTK_GRID(eq_grid), 6);
  static const char *eq_names[4] = { "Bass 100 Hz", "Mid 1500 Hz", "Treble 3000 Hz", "Volume" };
  for (int i = 0; i < 4; i++) {
    gtk_grid_attach(GTK_GRID(eq_grid), new_label(eq_names[i], GTK_ALIGN_END), 0, i, 1, 1);
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -20.0, 20.0, 0.5);
    gtk_scale_set_digits(GTK_SCALE(s), 1);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_scale_add_mark(GTK_SCALE(s), 0.0, GTK_POS_BOTTOM, NULL);
    gtk_range_set_value(GTK_RANGE(s), rade_glue_get_eq_gain(i));
    gtk_widget_set_size_request(s, 260, ROW_H);
    g_signal_connect(s, "value-changed", G_CALLBACK(eq_gain_cb), GINT_TO_POINTER(i));
    gtk_grid_attach(GTK_GRID(eq_grid), s, 1, i, 1, 1);
    eq_scale[i] = s;
  }
  GtkWidget *eq_pop;
  GtkWidget *eq_btn = popup_button("Adjust", eq_grid, &eq_pop);
  gtk_widget_set_tooltip_text(eq_btn, "EQ gains (used while EQ is on)");
  GtkWidget *eq_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(eq_box), opt_check[3], FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(eq_box), eq_btn, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), eq_box, 0, 2, 1, 1);         // EQ [Adjust] | Mute analog audio
  gtk_grid_attach(GTK_GRID(grid), opt_check[4], 1, 2, 1, 1);
  gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);
  //
  // Fit Filter, Quick Tune (the bands in a pop-up, as the SSTV panel) and the filter in use
  //
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *fit_b = gtk_button_new_with_label("Fit Filter");
  gtk_widget_set_size_request(fit_b, 110, ROW_H);
  g_signal_connect(fit_b, "clicked", G_CALLBACK(fit_cb), NULL);
  gtk_box_pack_start(GTK_BOX(row), fit_b, FALSE, FALSE, 0);
  GtkWidget *qt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *qt_pop;
  GtkWidget *qt_btn = popup_button("Quick Tune", qt_box, &qt_pop);
  for (int i = 0; i < rade_glue_quick_tune_count(); i++) {
    long long hz = 0;
    GtkWidget *b = gtk_button_new_with_label(rade_glue_quick_tune_label(i, &hz));
    gtk_widget_set_size_request(b, 72, 44);
    char tip[48];
    snprintf(tip, sizeof(tip), "%.3f MHz", hz / 1e6);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", G_CALLBACK(quick_tune_cb), GINT_TO_POINTER(i));
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(gtk_popover_popdown), qt_pop);
    gtk_box_pack_start(GTK_BOX(qt_box), b, FALSE, FALSE, 0);
    gtk_widget_show(b);
  }
  gtk_box_pack_start(GTK_BOX(row), qt_btn, FALSE, FALSE, 0);
  fit_label = new_label("", GTK_ALIGN_START);
  small_font(fit_label);
  gtk_box_pack_start(GTK_BOX(row), fit_label, FALSE, FALSE, 0);
  update_fit_label();
  gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
  return vbox;
}

int rade_menu_is_open(void) {
  return panel != NULL;
}

int rade_panel_wanted_width(int width, int height) {
  (void)height;
  return panel != NULL ? width / 2 : 0;   // as piHPSDR's two receivers side by side
}

void rade_panel_place(int x, int y, int w, int h) {
  if (panel == NULL || w <= 0) { return; }
  gtk_widget_set_size_request(panel, w, h);
  if (panel_x < 0) {
    gtk_fixed_put(GTK_FIXED(fixed), panel, x, y);
  } else if (x != panel_x || y != panel_y) {
    gtk_fixed_move(GTK_FIXED(fixed), panel, x, y);
  }
  panel_x = x;
  panel_y = y;
  gtk_widget_show_all(panel);
}

//
// program exit with the panel open: the zoom / pan from before RADE back
// (values only) so that radio_save_state() stores the user's settings; the
// rest of the RADE snapshot is rade_glue_shutdown's
//
void rade_menu_shutdown(void) {
  if (panel == NULL) { return; }
  if (timer_id) { g_source_remove(timer_id); timer_id = 0; }
  if (saved_zoom > 0 && receivers > 0 && receiver[0] != NULL) {
    receiver[0]->zoom = saved_zoom;
    receiver[0]->pan = saved_pan;
    saved_zoom = -1;
  }
}

//
// Main menu "RADE" (and the RADE toolbar / MIDI function): opening the panel
// enters RADE, opening it again closes the panel and leaves RADE
//
void rade_menu(GtkWidget *parent) {
  (void)parent;
  if (panel != NULL) {
    panel_close();
    return;
  }
#ifdef SSTV
  if (sstv_menu_is_open()) { sstv_menu(NULL); }   // one panel at a time on the right half
#endif
  if (!rade_glue_get_rx_enabled()) { rade_glue_set_rx_enabled(1); }   // entering RADE
  panel = build_panel();
  g_object_ref_sink(panel);
  if (receivers > 0 && receiver[0] != NULL) {
    saved_zoom = receiver[0]->zoom;
    saved_pan = receiver[0]->pan;
    radio_set_zoom(0, 26);
    center_pan();
    g_timeout_add(300, center_pan_cb, NULL);   // again once RADE's band / mode is in place
  }
  radio_reconfigure();        // spectrum / waterfall to the left, the panel at the right
  status_timer(NULL);
  timer_id = g_timeout_add(200, status_timer, NULL);
}

#endif /* RADE */
