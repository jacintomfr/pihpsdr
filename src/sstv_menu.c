/* SSTV window for piHPSDR-IF (phase 1: receive on the server's screen).
 *
 * The layout of hpsdr-rs's SSTV panel (digital_inapp.rs sstv_panel, RX tab):
 * the picture takes the window, a narrow column at its right holds three
 * vertical bars - Sync (green / amber / grey by quality: the tuning aid),
 * Lvl (input level), % (picture progress) - and under them what the
 * decoder is doing ("Scottie 1 (receiving)" / "(last)" / "hunting..."),
 * the station's FSK ID and an unsupported-mode note. Controls: Decode Auto
 * or one mode, Restart RX, Fit Filter (1100 - 2400 Hz), Auto-save, and the
 * Quick Tune calling frequencies. Decoding runs while this window is open.
 * Sized for the Pi's 1024x600 screen (picture 560x448 = 1.75x Scottie).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef SSTV

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "new_menu.h"
#include "radio.h"
#include "message.h"
#include "sstv_glue.h"
#include "sstv_menu.h"
#ifdef IFPAN
  #include "ifpan.h"
#endif

#define PIC_W 560
#define PIC_H 448
#define BARS_W 104

static GtkWidget *dialog = NULL;
static guint timer_id = 0;
static GtkWidget *area, *bars;
static GdkPixbuf *pix = NULL;
static uint8_t *rgb = NULL;
static size_t rgb_cap = 0;
static uint32_t last_image_id = 0xffffffffu;
static sstv_status_t st;

// hpsdr-rs SSTV_QUICK_TUNE_HZ (community calling frequencies, a starting point)
static const struct { const char *label; long long hz; } quick[] = {
  { "80m", 3730000LL }, { "40m", 7033000LL }, { "30m", 10132000LL }, { "20m", 14230000LL }, { "15m", 21340000LL },
};

//
// The SSTV window opens where it was last (like the RADE window)
//
#define SSTV_WINDOW_POS_FILE "sstv-window.txt"
static int pos_x = 0, pos_y = 0, pos_have = -1;   // -1: file not read yet

static void pos_save(void) {
  FILE *fp = fopen(SSTV_WINDOW_POS_FILE, "w");
  if (fp) {
    fprintf(fp, "%d %d\n", pos_x, pos_y);
    fclose(fp);
  }
}

static void pos_load(void) {
  if (pos_have >= 0) { return; }
  pos_have = 0;
  FILE *fp = fopen(SSTV_WINDOW_POS_FILE, "r");
  if (fp) {
    if (fscanf(fp, "%d %d", &pos_x, &pos_y) == 2) { pos_have = 1; }
    fclose(fp);
  }
}

static int pos_ok(int x, int y) {
#ifdef IFPAN
  return ifpan_point_on_screen(x, y);
#else
  return x > -10000 && y > -10000;
#endif
}

static gboolean configure_cb(GtkWidget *widget, GdkEvent *event, gpointer data) {
  int x, y;
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  if ((x != 0 || y != 0) && pos_ok(x, y)) {
    pos_x = x;
    pos_y = y;
    pos_have = 1;
  }
  return FALSE;
}

static gboolean pos_apply(gpointer data) {
  if (dialog != NULL && pos_have == 1) { gtk_window_move(GTK_WINDOW(dialog), pos_x, pos_y); }
  return G_SOURCE_REMOVE;
}

static void cleanup(void) {
  if (timer_id) { g_source_remove(timer_id); timer_id = 0; }
  if (dialog != NULL && pos_have == 1) { pos_save(); }
  if (dialog != NULL) {
    GtkWidget *tmp = dialog;
    dialog = NULL;
    sstv_glue_set_rx_enabled(0);
    gtk_widget_destroy(tmp);
    sub_menu = NULL;
    active_menu  = NO_MENU;
    radio_save_state();
  }
  if (pix) { g_object_unref(pix); pix = NULL; }
  last_image_id = 0xffffffffu;
}

static gboolean close_cb(void) {
  cleanup();
  return TRUE;
}

static gboolean destroy_cb(void) {
  cleanup();
  return TRUE;
}

// the picture, as large as the area allows with its aspect ratio
static gboolean pic_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data) {
  int aw = gtk_widget_get_allocated_width(widget);
  int ah = gtk_widget_get_allocated_height(widget);
  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_paint(cr);
  if (pix == NULL) {
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
    cairo_set_font_size(cr, 16.0);
    cairo_move_to(cr, 16.0, 30.0);
    cairo_show_text(cr, "No picture yet -- waiting for a VIS header.");
    return FALSE;
  }
  int pw = gdk_pixbuf_get_width(pix);
  int ph = gdk_pixbuf_get_height(pix);
  double k = (double) aw / pw;
  if ((double) ah / ph < k) { k = (double) ah / ph; }
  cairo_scale(cr, k, k);   // top-left, as hpsdr-rs
  gdk_cairo_set_source_pixbuf(cr, pix, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  return FALSE;
}

static void text_center(cairo_t *cr, double x, double y, const char *t) {
  cairo_text_extents_t e;
  cairo_text_extents(cr, t, &e);
  cairo_move_to(cr, x - e.x_advance / 2.0, y);
  cairo_show_text(cr, t);
}

// right column: three vertical bars (hpsdr-rs: 28 px wide, 36 px apart, value from the bottom)
static gboolean bars_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data) {
  int ah = gtk_widget_get_allocated_height(widget);
  double bar_h = ah - 22.0;
  char pct[16];
  snprintf(pct, sizeof(pct), "%.0f%%", 100.0 * (st.receiving ? st.progress : 0.0));
  double sq = st.sync_quality;
  double sc[3];
  if (sq > 0.7) {
    sc[0] = 40 / 255.0; sc[1] = 190 / 255.0; sc[2] = 70 / 255.0;        // green
  } else if (sq > 0.3) {
    sc[0] = 230 / 255.0; sc[1] = 150 / 255.0; sc[2] = 50 / 255.0;       // amber
  } else {
    sc[0] = sc[1] = sc[2] = 140 / 255.0;                                  // grey
  }
  struct { const char *label; double v; double r, g, b; } b[3] = {
    { "Sync", sq, sc[0], sc[1], sc[2] },
    { "Lvl", st.level, 90 / 255.0, 160 / 255.0, 230 / 255.0 },
    { pct, st.receiving ? st.progress : 0.0, 230 / 255.0, 150 / 255.0, 50 / 255.0 },
  };
  cairo_set_font_size(cr, 13.0);
  for (int i = 0; i < 3; i++) {
    double x = 2.0 + i * 36.0;
    double v = b[i].v < 0.0 ? 0.0 : b[i].v > 1.0 ? 1.0 : b[i].v;
    cairo_set_source_rgb(cr, 20 / 255.0, 20 / 255.0, 20 / 255.0);
    cairo_rectangle(cr, x, 0, 28, bar_h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, b[i].r, b[i].g, b[i].b);
    cairo_rectangle(cr, x, bar_h * (1.0 - v), 28, bar_h * v);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 70 / 255.0, 70 / 255.0, 70 / 255.0);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, 0.5, 27, bar_h - 1);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 190 / 255.0, 190 / 255.0, 190 / 255.0);
    text_center(cr, x + 14.0, bar_h + 15.0, b[i].label);
  }
  return FALSE;
}

static GtkWidget *state_dot, *state_label, *station_label, *unsupported_label;

static gboolean dot_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data) {
  if (st.receiving) {
    cairo_set_source_rgb(cr, 40 / 255.0, 190 / 255.0, 70 / 255.0);
  } else {
    cairo_set_source_rgb(cr, 230 / 255.0, 150 / 255.0, 50 / 255.0);
  }
  cairo_arc(cr, 6.0, 9.0, 5.0, 0, 2 * G_PI);
  cairo_fill(cr);
  return FALSE;
}

static gboolean status_timer(gpointer data) {
  const sstv_t *e = sstv_glue_engine();
  if (dialog == NULL || e == NULL) { return G_SOURCE_CONTINUE; }
  memset(&st, 0, sizeof(st));
  sstv_get_status(e, &st);
  // the picture: while one is being decoded, and once when a new one starts / ends
  if (st.w > 0 && st.h > 0 && (st.receiving || st.image_id != last_image_id)) {
    size_t need = (size_t) st.w * st.h * 3;
    if (rgb_cap < need) {
      g_free(rgb);
      rgb = g_malloc(need);
      rgb_cap = need;
    }
    if (sstv_copy_rgb(e, rgb, rgb_cap) == need) {
      if (pix == NULL || gdk_pixbuf_get_width(pix) != st.w || gdk_pixbuf_get_height(pix) != st.h) {
        if (pix) { g_object_unref(pix); }
        pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, st.w, st.h);
      }
      int stride = gdk_pixbuf_get_rowstride(pix);
      guchar *p = gdk_pixbuf_get_pixels(pix);
      for (int y = 0; y < st.h; y++) {
        memcpy(p + (size_t) y * stride, rgb + (size_t) y * st.w * 3, (size_t) st.w * 3);
      }
      gtk_widget_queue_draw(area);
    }
    last_image_id = st.image_id;
  } else if (st.w == 0 && pix != NULL && st.image_id != last_image_id) {
    g_object_unref(pix);       // Restart RX: picture cleared
    pix = NULL;
    last_image_id = st.image_id;
    gtk_widget_queue_draw(area);
  }
  char t[96];
  const char *m = st.detected >= 0 ? sstv_mode_label(st.detected) : NULL;
  if (m) {
    snprintf(t, sizeof(t), "%s\n%s", m, st.receiving ? "(receiving)" : "(last)");
  } else {
    snprintf(t, sizeof(t), "hunting...");
  }
  gtk_label_set_text(GTK_LABEL(state_label), t);
  if (st.rx_id[0]) {
    snprintf(t, sizeof(t), "Station: %s", st.rx_id);
    gtk_label_set_text(GTK_LABEL(station_label), t);
  } else {
    gtk_label_set_text(GTK_LABEL(station_label), "");
  }
  if (st.unsupported[0]) {
    snprintf(t, sizeof(t), "<span foreground=\"#e69632\">%s: not decoded</span>", st.unsupported);
    gtk_label_set_markup(GTK_LABEL(unsupported_label), t);
  } else {
    gtk_label_set_text(GTK_LABEL(unsupported_label), "");
  }
  gtk_widget_queue_draw(bars);
  gtk_widget_queue_draw(state_dot);
  return G_SOURCE_CONTINUE;
}

static GtkWidget *auto_btn, *mode_combo;
static int updating = 0;

static void auto_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  updating = 1;
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
    sstv_glue_set_mode(-1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), -1);
  } else if (sstv_glue_get_mode() < 0) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);   // pick a mode to leave Auto
  }
  updating = 0;
}

static void mode_cb(GtkWidget *widget, gpointer data) {
  if (updating) { return; }
  int m = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  if (m < 0) { return; }
  updating = 1;
  sstv_glue_set_mode(m);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_btn), FALSE);
  updating = 0;
}

static void restart_cb(GtkWidget *widget, gpointer data) {
  const sstv_t *e = sstv_glue_engine();
  if (e) { sstv_restart(e); }
}

static void fit_cb(GtkWidget *widget, gpointer data) {
  sstv_glue_fit_filter();
}

static void save_cb(GtkWidget *widget, gpointer data) {
  sstv_glue_set_auto_save(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

static void quick_cb(GtkWidget *widget, gpointer data) {
  sstv_glue_quick_tune(quick[GPOINTER_TO_INT(data)].hz);
}

void sstv_menu(GtkWidget *parent) {
  dialog = gtk_dialog_new();
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
  GtkWidget *headerbar = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(dialog), headerbar);
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(headerbar), TRUE);
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerbar), APP_NAME " - SSTV");
  g_signal_connect(dialog, "delete_event", G_CALLBACK(close_cb), NULL);
  g_signal_connect(dialog, "destroy", G_CALLBACK(destroy_cb), NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  //
  // control strip: Close, Decode (Auto / one mode), Restart RX, Fit Filter, Auto-save
  //
  GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *close_b = gtk_button_new_with_label("Close");
  gtk_widget_set_name(close_b, "close_button");
  g_signal_connect(close_b, "button-press-event", G_CALLBACK(close_cb), NULL);
  gtk_box_pack_start(GTK_BOX(strip), close_b, FALSE, FALSE, 0);
  if (radio_is_remote) {
    // phase 1: the decoder and the picture are on the server
    GtkWidget *l = gtk_label_new("SSTV runs on the server for now: open SSTV on the piHPSDR-IF server screen.");
    gtk_widget_set_name(l, "boldlabel");
    gtk_box_pack_start(GTK_BOX(strip), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), strip, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(content), vbox);
    sub_menu = dialog;
    gtk_widget_show_all(dialog);
    return;
  }
  GtkWidget *l = gtk_label_new("Decode:");
  gtk_widget_set_name(l, "boldlabel");
  gtk_box_pack_start(GTK_BOX(strip), l, FALSE, FALSE, 0);
  auto_btn = gtk_toggle_button_new_with_label("Auto");
  gtk_widget_set_tooltip_text(auto_btn, "Mode from the VIS header (or the sync cadence)");
  gtk_box_pack_start(GTK_BOX(strip), auto_btn, FALSE, FALSE, 0);
  mode_combo = gtk_combo_box_text_new();
  for (int i = 0; i < sstv_mode_count(); i++) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode_combo), NULL, sstv_mode_label(i));
  }
  gtk_widget_set_tooltip_text(mode_combo, "Decode only this mode (free-run, also without a VIS header)");
  gtk_box_pack_start(GTK_BOX(strip), mode_combo, FALSE, FALSE, 0);
  updating = 1;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_btn), sstv_glue_get_mode() < 0);
  gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), sstv_glue_get_mode());
  updating = 0;
  g_signal_connect(auto_btn, "toggled", G_CALLBACK(auto_cb), NULL);
  g_signal_connect(mode_combo, "changed", G_CALLBACK(mode_cb), NULL);
  GtkWidget *restart = gtk_button_new_with_label("Restart RX");
  gtk_widget_set_tooltip_text(restart, "Drop the picture in progress and wait for the next header");
  g_signal_connect(restart, "clicked", G_CALLBACK(restart_cb), NULL);
  gtk_box_pack_start(GTK_BOX(strip), restart, FALSE, FALSE, 0);
  GtkWidget *fit = gtk_button_new_with_label("Fit Filter");
  gtk_widget_set_tooltip_text(fit, "Narrow the RX filter to the SSTV tone band (1100-2400 Hz)");
  g_signal_connect(fit, "clicked", G_CALLBACK(fit_cb), NULL);
  gtk_box_pack_start(GTK_BOX(strip), fit, FALSE, FALSE, 0);
  GtkWidget *save = gtk_check_button_new_with_label("Auto-save");
  gtk_widget_set_name(save, "boldlabel");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(save), sstv_glue_get_auto_save());
  gtk_widget_set_tooltip_text(save, "Save every completed picture as a PNG, like QSSTV does -- sstv_images/");
  g_signal_connect(save, "toggled", G_CALLBACK(save_cb), NULL);
  gtk_box_pack_start(GTK_BOX(strip), save, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), strip, FALSE, FALSE, 0);
  //
  // the picture on the left, the bars and the decoder state on the right
  //
  GtkWidget *main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  area = gtk_drawing_area_new();
  gtk_widget_set_size_request(area, PIC_W, PIC_H);
  gtk_widget_set_hexpand(area, TRUE);
  gtk_widget_set_vexpand(area, TRUE);
  g_signal_connect(area, "draw", G_CALLBACK(pic_draw_cb), NULL);
  gtk_box_pack_start(GTK_BOX(main), area, TRUE, TRUE, 0);
  GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_size_request(col, BARS_W, -1);
  bars = gtk_drawing_area_new();
  gtk_widget_set_size_request(bars, BARS_W, PIC_H - 22 - 64);
  gtk_widget_set_vexpand(bars, TRUE);
  g_signal_connect(bars, "draw", G_CALLBACK(bars_draw_cb), NULL);
  gtk_box_pack_start(GTK_BOX(col), bars, TRUE, TRUE, 0);
  GtkWidget *srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  state_dot = gtk_drawing_area_new();
  gtk_widget_set_size_request(state_dot, 12, 18);
  gtk_widget_set_valign(state_dot, GTK_ALIGN_START);
  g_signal_connect(state_dot, "draw", G_CALLBACK(dot_draw_cb), NULL);
  gtk_box_pack_start(GTK_BOX(srow), state_dot, FALSE, FALSE, 0);
  state_label = gtk_label_new("hunting...");
  gtk_label_set_line_wrap(GTK_LABEL(state_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(state_label), 0.0);
  gtk_box_pack_start(GTK_BOX(srow), state_label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(col), srow, FALSE, FALSE, 0);
  station_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(station_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(station_label), 0.0);
  gtk_box_pack_start(GTK_BOX(col), station_label, FALSE, FALSE, 0);
  unsupported_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(unsupported_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(unsupported_label), 0.0);
  gtk_box_pack_start(GTK_BOX(col), unsupported_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(main), col, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), main, TRUE, TRUE, 0);
  //
  // Quick Tune: the SSTV calling frequencies
  //
  GtkWidget *qrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  l = gtk_label_new("Quick Tune:");
  gtk_widget_set_name(l, "boldlabel");
  gtk_box_pack_start(GTK_BOX(qrow), l, FALSE, FALSE, 0);
  for (int i = 0; i < (int)(sizeof(quick) / sizeof(quick[0])); i++) {
    GtkWidget *b = gtk_button_new_with_label(quick[i].label);
    char tip[64];
    snprintf(tip, sizeof(tip), "%.3f MHz - a starting point, not a standard", quick[i].hz / 1e6);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", G_CALLBACK(quick_cb), GINT_TO_POINTER(i));
    gtk_box_pack_start(GTK_BOX(qrow), b, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(vbox), qrow, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(content), vbox);
  sub_menu = dialog;
  sstv_glue_set_rx_enabled(1);
  pos_load();
  int have = (pos_have == 1 && pos_ok(pos_x, pos_y));
  int want_x = pos_x, want_y = pos_y;
  if (have) { gtk_window_move(GTK_WINDOW(dialog), want_x, want_y); }
  gtk_widget_show_all(dialog);
  if (have) {
    pos_x = want_x;
    pos_y = want_y;
    g_timeout_add(150, pos_apply, NULL);
  }
  g_signal_connect(dialog, "configure-event", G_CALLBACK(configure_cb), NULL);
  status_timer(NULL);
  timer_id = g_timeout_add(200, status_timer, NULL);
}

#endif /* SSTV */
