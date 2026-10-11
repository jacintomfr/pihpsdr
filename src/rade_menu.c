/* Copyright (C)
* RADE V1 menu for piHPSDR-IF (UI like the hpsdr-rs RADE panel), ported from
* the deskHPSDR Windows RADE port. Differences: own callsign edited here
* (piHPSDR has no "own callsign"), no FreeDV Reporter yet, Quick Tune moves
* the Kenwood (band change by CAT), settings saved when the menu closes.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifdef RADE

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "new_menu.h"
#include "filter.h"
#include "mode.h"
#include "radio.h"
#include "receiver.h"
#include "vfo.h"
#include "ext.h"
#include "rade_glue.h"
#include "rade_menu.h"
#ifdef IFPAN
  #include "ifpan.h"
#endif

static GtkWidget *dialog = NULL;
static guint timer_id = 0;
static int updating = 0;
static int fit_applied = 0;   /* Var1 filter was set by Fit Filter: follow mode changes */

static GtkWidget *rx_check, *tx_check, *status_label, *level_bar, *calls_label, *fit_label;

static const struct { const char *label; long long hz; } quick_tune[] = {
  {"80m", 3630000LL}, {"40m", 7180000LL}, {"20m", 14236000LL},
  {"15m", 21180000LL}, {"10m", 28330000LL}
};

static void cleanup(void) {
  if (timer_id) { g_source_remove(timer_id); timer_id = 0; }
  if (dialog != NULL) {
    GtkWidget *tmp = dialog;
    dialog = NULL;
    gtk_widget_destroy(tmp);
    sub_menu = NULL;
    active_menu  = NO_MENU;
    radio_save_state();
  }
}

static gboolean close_cb(void) {
  cleanup();
  return TRUE;
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
  rade_glue_set_eq_gain(GPOINTER_TO_INT(data), (float) gtk_range_get_value(range));
}

static void update_fit_label(void) {
  gtk_label_set_text(GTK_LABEL(fit_label), rade_glue_get_filter_wide()
                     ? "RX filter: 300-2700 Hz (wide)" : "RX filter: 700-2300 Hz (narrow)");
}

/*
 * Set the Var1 filter of the active receiver's VFO to the RADE passband.
 * Var1 edges are in the CAT ZZFL/ZZFH convention: (-high, -low) for the
 * lower sideband mode DIGL.  The TX bandpass is forced to 700-2300 Hz by
 * rade_glue while TX is armed, so it is not touched here.
 */
static void apply_fit_filter(void) {
  int id = active_receiver->id;
  int mode = vfo[id].mode;
  if (mode != modeDIGU && mode != modeDIGL) { return; }
  int low = rade_glue_get_filter_wide() ? 300 : 700;
  int high = rade_glue_get_filter_wide() ? 2700 : 2300;
  FILTER *f = &filters[mode][filterVar1];
  if (mode == modeDIGL) {
    f->low = -high;
    f->high = -low;
  } else {
    f->low = low;
    f->high = high;
  }
  vfo_id_filter_changed(id, filterVar1);
  fit_applied = 1;
  g_idle_add(ext_vfo_update, NULL);
}

static void fit_cb(GtkWidget *widget, gpointer data) {
  (void)widget;
  (void)data;
  int mode = vfo[active_receiver->id].mode;
  if (mode != modeDIGU && mode != modeDIGL) {
    gtk_label_set_text(GTK_LABEL(fit_label), "Fit Filter needs DIGU/DIGL mode");
    return;
  }
  // rotate wide <-> narrow
  rade_glue_set_filter_wide(!rade_glue_get_filter_wide());
  apply_fit_filter();
  update_fit_label();
}

static void quick_tune_cb(GtkWidget *widget, gpointer data) {
  (void)widget;
  long long hz = quick_tune[GPOINTER_TO_INT(data)].hz;
  int id = active_receiver->id;
  // lower sideband below 10 MHz (digi_mode_for_band of the Rust app)
  int mode = (hz < 10000000LL) ? modeDIGL : modeDIGU;
  if (vfo[id].mode != mode) {
    vfo_id_mode_changed(id, mode);
  }
#ifdef IFPAN
  // the Kenwood is tuned (frequency and USB / LSB) and the display follows it
  ifpan_band_changed(hz, mode);
#else
  vfo_id_set_frequency(id, hz);
#endif
  if (fit_applied) { apply_fit_filter(); }   // keep the fitted width, sign follows the mode
  g_idle_add(ext_vfo_update, NULL);
}

static gboolean status_timer(gpointer data) {
  (void)data;
  if (dialog == NULL) { timer_id = 0; return G_SOURCE_REMOVE; }
  sync_checks();
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
  char list[RADE_GLUE_HISTORY * 40];
  list[0] = 0;
  int n = rade_glue_callsign_count();
  for (int i = 0; i < n; i++) {
    char call[9];
    float snr;
    if (rade_glue_get_callsign(i, call, &snr)) {
      char line[40];
      snprintf(line, sizeof(line), "%-8s  %+.1f dB\n", call, snr);
      strncat(list, line, sizeof(list) - strlen(list) - 1);
    }
  }
  gtk_label_set_text(GTK_LABEL(calls_label), n ? list : "(none yet)");
  return G_SOURCE_CONTINUE;
}

static GtkWidget *add_check(GtkWidget *grid, const char *text, int on, GCallback cb, int col, int row) {
  GtkWidget *b = gtk_check_button_new_with_label(text);
  gtk_widget_set_name(b, "boldlabel");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), on);
  gtk_grid_attach(GTK_GRID(grid), b, col, row, 1, 1);
  g_signal_connect(b, "toggled", cb, NULL);
  return b;
}

static GtkWidget *add_label(GtkWidget *grid, const char *text, GtkAlign align, int col, int row, int span) {
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_set_name(l, "boldlabel");
  gtk_widget_set_halign(l, align);
  gtk_grid_attach(GTK_GRID(grid), l, col, row, span, 1);
  return l;
}

static GtkWidget *new_dialog(GtkWidget *parent, GtkWidget **grid) {
  GtkWidget *d = gtk_dialog_new();
  gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(parent));
  GtkWidget *headerbar = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(d), headerbar);
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(headerbar), TRUE);
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerbar), APP_NAME " - RADE");
  g_signal_connect(d, "delete_event", G_CALLBACK(close_cb), NULL);
  g_signal_connect(d, "destroy", G_CALLBACK(close_cb), NULL);
  *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(*grid), 10);
  gtk_grid_set_row_spacing(GTK_GRID(*grid), 6);
  GtkWidget *close_b = gtk_button_new_with_label("Close");
  gtk_widget_set_name(close_b, "close_button");
  g_signal_connect(close_b, "button-press-event", G_CALLBACK(close_cb), NULL);
  gtk_grid_attach(GTK_GRID(*grid), close_b, 0, 0, 1, 1);
  return d;
}

void rade_menu(GtkWidget *parent) {
  GtkWidget *grid;
  dialog = new_dialog(parent, &grid);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  int row = 0;
  if (radio_is_remote) {
    //
    // RADE runs on the server (next to the radio); its controls on the
    // client come with the client/server part (next step).
    //
    add_label(grid, "RADE runs on the server: use the RADE menu there.\n"
              "Client control of RADE is not available yet.", GTK_ALIGN_START, 1, row, 2);
    gtk_container_add(GTK_CONTAINER(content), grid);
    sub_menu = dialog;
    gtk_widget_show_all(dialog);
    return;
  }
  rx_check = add_check(grid, "Enable RADE RX", rade_glue_get_rx_enabled(), G_CALLBACK(rx_cb), 1, row);
  tx_check = add_check(grid, "TX armed", rade_glue_get_tx_armed(), G_CALLBACK(tx_cb), 2, row);
  if (transmitter == NULL) { gtk_widget_set_sensitive(tx_check, FALSE); }
  row++;
  status_label = add_label(grid, "", GTK_ALIGN_START, 0, row, 3);
  gtk_label_set_width_chars(GTK_LABEL(status_label), 60);
  row++;
  add_label(grid, "Level", GTK_ALIGN_END, 0, row, 1);
  level_bar = gtk_progress_bar_new();
  gtk_widget_set_hexpand(level_bar, TRUE);
  gtk_grid_attach(GTK_GRID(grid), level_bar, 1, row, 2, 1);
  row++;
  GtkWidget *rc = add_label(grid, "Received callsigns:", GTK_ALIGN_END, 0, row, 1);
  gtk_widget_set_valign(rc, GTK_ALIGN_START);
  calls_label = add_label(grid, "(none yet)", GTK_ALIGN_START, 1, row, 2);
  gtk_label_set_width_chars(GTK_LABEL(calls_label), 24);
  row++;
  add_label(grid, "My callsign:", GTK_ALIGN_END, 0, row, 1);
  GtkWidget *call_entry = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(call_entry), 8);
  gtk_entry_set_text(GTK_ENTRY(call_entry), rade_glue_get_callsign_own());
  gtk_widget_set_tooltip_text(call_entry, "Sent in the End-of-Over frame of every RADE transmission");
  g_signal_connect(call_entry, "changed", G_CALLBACK(callsign_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), call_entry, 1, row, 1, 1);
  row++;
  add_check(grid, "Denoiser", rade_glue_get_denoiser(), G_CALLBACK(denoiser_cb), 0, row);
  add_check(grid, "Leveler", rade_glue_get_leveler(), G_CALLBACK(leveler_cb), 1, row);
  add_check(grid, "Compressor", rade_glue_get_compressor(), G_CALLBACK(compressor_cb), 2, row);
  row++;
  add_check(grid, "EQ", rade_glue_get_eq(), G_CALLBACK(eq_cb), 0, row);
  add_check(grid, "Mute analog audio", rade_glue_get_mute_analog(), G_CALLBACK(mute_cb), 1, row);
  add_check(grid, "Squelch gate", rade_glue_get_squelch(), G_CALLBACK(squelch_cb), 2, row);
  row++;
  // EQ gains (apply only while the EQ check box is on): bass 100 Hz, mid 1500 Hz, treble 3000 Hz, output volume
  static const char *eq_names[4] = { "EQ Bass 100 Hz", "EQ Mid 1500 Hz", "EQ Treble 3000 Hz", "EQ Volume" };
  for (int i = 0; i < 4; i++) {
    add_label(grid, eq_names[i], GTK_ALIGN_END, 0, row, 1);
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -20.0, 20.0, 0.5);
    gtk_scale_set_digits(GTK_SCALE(s), 1);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_scale_add_mark(GTK_SCALE(s), 0.0, GTK_POS_BOTTOM, NULL);
    gtk_range_set_value(GTK_RANGE(s), rade_glue_get_eq_gain(i));
    gtk_widget_set_hexpand(s, TRUE);
    g_signal_connect(s, "value-changed", G_CALLBACK(eq_gain_cb), GINT_TO_POINTER(i));
    gtk_grid_attach(GTK_GRID(grid), s, 1, row, 2, 1);
    row++;
  }
  GtkWidget *fit_b = gtk_button_new_with_label("Fit Filter");
  g_signal_connect(fit_b, "clicked", G_CALLBACK(fit_cb), NULL);
  gtk_grid_attach(GTK_GRID(grid), fit_b, 0, row, 1, 1);
  fit_label = add_label(grid, "", GTK_ALIGN_START, 1, row, 2);
  update_fit_label();
  row++;
  add_label(grid, "Quick Tune:", GTK_ALIGN_END, 0, row, 1);
  GtkWidget *qt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  for (int i = 0; i < 5; i++) {
    GtkWidget *b = gtk_button_new_with_label(quick_tune[i].label);
    char tip[48];
    snprintf(tip, sizeof(tip), "%.3f MHz", quick_tune[i].hz / 1e6);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", G_CALLBACK(quick_tune_cb), GINT_TO_POINTER(i));
    gtk_box_pack_start(GTK_BOX(qt_box), b, TRUE, TRUE, 0);
  }
  gtk_grid_attach(GTK_GRID(grid), qt_box, 1, row, 2, 1);
  gtk_container_add(GTK_CONTAINER(content), grid);
  sub_menu = dialog;
  gtk_widget_show_all(dialog);
  status_timer(NULL);
  timer_id = g_timeout_add(200, status_timer, NULL);
}

#endif /* RADE */
