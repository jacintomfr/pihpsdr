/* SSTV window for piHPSDR-IF (phase 1: receive on the server's screen).
 *
 * The layout of hpsdr-rs's SSTV panel (digital_inapp.rs sstv_panel, RX tab):
 * the picture takes the window, a narrow column at its right holds three
 * vertical bars - Sync (green / amber / grey by quality: the tuning aid),
 * Lvl (input level), % (picture progress) - and under them what the
 * decoder is doing ("Scottie 1 (receiving)" / "(last)" / "hunting..."),
 * the station's FSK ID and an unsupported-mode note. Controls: Decode Auto
 * or one mode, Restart RX, Fit Filter (1100 - 2400 Hz), Auto-save, and the
 * Quick Tune calling frequencies. Decoding runs while the panel is open.
 *
 * The panel is docked at the right of the spectrum / waterfall, which
 * shrink to the left the way piHPSDR puts two receivers side by side
 * (radio_reconfigure asks sstv_panel_wanted_width, then sstv_panel_place):
 * nothing of the spectrum is covered. Its width follows the height there
 * is, so the picture (5:4, Scottie / Martin) is as large as it can be.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef SSTV

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "new_menu.h"
#include "radio.h"
#include "receiver.h"
#include "vfo.h"
#include "mode.h"
#include "message.h"
#include "sstv_glue.h"
#include "sstv_menu.h"
#ifdef IFPAN
  #include "ifpan.h"
#endif

#define BARS_W 104
#define STATE_CHARS 11      // "(receiving)", the widest state text

static GtkWidget *panel = NULL;      // docked panel (in the main window's GtkFixed)
static int panel_x = -1, panel_y = -1, panel_w = 0, panel_h = 0;
static guint timer_id = 0;
static GtkWidget *area, *bars;
static GdkPixbuf *pix = NULL;
static uint8_t *rgb = NULL;
static size_t rgb_cap = 0;
static uint32_t last_image_id = 0xffffffffu;
static int saved_zoom = -1;          // RX1 zoom before the panel opened (zoom 26 while it is open)
static int saved_pan = 0;

//
// pan RX1 so that the middle of the SSTV band (1750 Hz audio, between the
// 1100 Hz and 2400 Hz edges) is in the centre of the panadapter; the pan
// steps are coarse (sample rate * (zoom - 1) / zoom / 200, ~460 Hz at 96 kHz)
//
static void center_pan(void) {
  if (receivers < 1 || receiver[0] == NULL || receiver[0]->zoom <= 1) { return; }
  const RECEIVER *rx = receiver[0];
  int m = vfo[0].mode;
  double sign = (m == modeLSB || m == modeDIGL) ? -1.0 : 1.0;
  long long f = vfo[0].ctun ? vfo[0].ctun_frequency : vfo[0].frequency;
  double offset = (double)(f - vfo[0].frequency) + sign * 1750.0 + rx->sample_rate / 2.0;
  double z = (offset * rx->zoom / (double)rx->sample_rate - 0.5) / (double)(rx->zoom - 1);
  radio_set_pan(0, (int)lround(200.0 * z) - 100);
}

static gboolean center_pan_cb(gpointer data) {
  if (panel != NULL) { center_pan(); }
  return G_SOURCE_REMOVE;
}
static sstv_status_t st;

// hpsdr-rs SSTV_QUICK_TUNE_HZ (community calling frequencies, a starting point)
static const struct { const char *label; long long hz; } quick[] = {
  { "80m", 3730000LL }, { "40m", 7033000LL }, { "30m", 10132000LL }, { "20m", 14230000LL }, { "15m", 21340000LL },
};

static void panel_close(void) {
  if (timer_id) { g_source_remove(timer_id); timer_id = 0; }
  if (panel != NULL) {
    GtkWidget *tmp = panel;
    panel = NULL;
    sstv_glue_set_rx_enabled(0);
    sstv_glue_filter_restore(); // the filter from before SSTV
    gtk_widget_destroy(tmp);
    g_object_unref(tmp);      // the reference taken in sstv_menu
    if (saved_zoom > 0) {
      radio_set_zoom(0, saved_zoom);   // back to the zoom from before SSTV
      radio_set_pan(0, saved_pan);
      saved_zoom = -1;
    }
    radio_reconfigure();      // the spectrum / waterfall get the whole width back
    radio_save_state();
  }
  if (pix) { g_object_unref(pix); pix = NULL; }
  last_image_id = 0xffffffffu;
  panel_x = panel_y = -1;
}

static gboolean close_cb(void) {
  panel_close();
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
  if (panel == NULL || e == NULL) { return G_SOURCE_CONTINUE; }
  memset(&st, 0, sizeof(st));
  sstv_get_status(e, &st);
  // the picture: while one is being decoded, once when a new one starts, and
  // once more when it ends -- MMSSTV's receiver redraws the whole picture
  // (resync / slant) right up to its last line, in the same step that ends it
  static int was_receiving = 0;
  int ended = was_receiving && !st.receiving;
  was_receiving = st.receiving;
  if (st.w > 0 && st.h > 0 && (st.receiving || ended || st.image_id != last_image_id)) {
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
  g_timeout_add(300, center_pan_cb, NULL);   // once the new frequency is in place
}

//
// radio_reconfigure: the width the panel wants at the right (0: not docked):
// the picture area as high as the receiver area minus the two control rows,
// 5:4 wide, plus the bars column; at most 60 % of the screen
//
int sstv_panel_wanted_width(int width, int height) {
  if (panel == NULL) { return 0; }
  int pic_h = height - 2 * 44 - 12;
  int w = pic_h * 5 / 4 + BARS_W + 8 + 12;
  if (w > width * 6 / 10) { w = width * 6 / 10; }
  if (w < 320) { w = 320; }
  return w;
}

void sstv_panel_place(int x, int y, int w, int h) {
  if (panel == NULL || w <= 0) { return; }
  gtk_widget_set_size_request(panel, w, h);
  if (panel_x < 0) {
    gtk_fixed_put(GTK_FIXED(fixed), panel, x, y);
  } else if (x != panel_x || y != panel_y) {
    gtk_fixed_move(GTK_FIXED(fixed), panel, x, y);
  }
  panel_x = x;
  panel_y = y;
  panel_w = w;
  panel_h = h;
  gtk_widget_show_all(panel);
}

static GtkWidget *build_panel(void) {
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
  //
  // control strip: Close, Decode (Auto / one mode), Restart RX, Fit Filter, Auto-save
  //
  GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *close_b = gtk_button_new_with_label("Close");
  gtk_widget_set_name(close_b, "close_button");
  g_signal_connect(close_b, "button-press-event", G_CALLBACK(close_cb), NULL);
  gtk_box_pack_start(GTK_BOX(strip), close_b, FALSE, FALSE, 0);
  GtkWidget *l = gtk_label_new("Decode:");
  gtk_widget_set_name(l, "boldlabel");
  gtk_box_pack_start(GTK_BOX(strip), l, FALSE, FALSE, 0);
  auto_btn = gtk_toggle_button_new_with_label("Auto");
  {
    // Auto on: green, unmistakable (the theme's "checked" grey is too faint)
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
                                    "button:checked { background-image: none; background-color: rgb(20%, 75%, 20%); color: rgb(0%, 0%, 0%); }",
                                    -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(auto_btn), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
  }
  gtk_widget_set_tooltip_text(auto_btn, "Mode from the VIS header (or the sync rhythm of a picture already running)");
  gtk_box_pack_start(GTK_BOX(strip), auto_btn, FALSE, FALSE, 0);
  mode_combo = gtk_combo_box_text_new();
  for (int i = 0; i < sstv_mode_count(); i++) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode_combo), NULL, sstv_mode_label(i));
  }
  gtk_widget_set_tooltip_text(mode_combo, "Receive now in this mode (forced start, as in MMSSTV)");
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
  gtk_widget_set_size_request(area, 160, 128);
  gtk_widget_set_hexpand(area, TRUE);
  gtk_widget_set_vexpand(area, TRUE);
  g_signal_connect(area, "draw", G_CALLBACK(pic_draw_cb), NULL);
  gtk_box_pack_start(GTK_BOX(main), area, TRUE, TRUE, 0);
  GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_size_request(col, BARS_W, -1);
  bars = gtk_drawing_area_new();
  gtk_widget_set_size_request(bars, BARS_W, 120);
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
  // fixed width: "(receiving)" / "(last)" / a station's call must not resize
  // the column (and the picture with it); longer text wraps
  gtk_label_set_width_chars(GTK_LABEL(state_label), STATE_CHARS);
  gtk_label_set_max_width_chars(GTK_LABEL(state_label), STATE_CHARS);
  gtk_label_set_xalign(GTK_LABEL(state_label), 0.0);
  gtk_box_pack_start(GTK_BOX(srow), state_label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(col), srow, FALSE, FALSE, 0);
  station_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(station_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(station_label), STATE_CHARS);
  gtk_label_set_xalign(GTK_LABEL(station_label), 0.0);
  gtk_box_pack_start(GTK_BOX(col), station_label, FALSE, FALSE, 0);
  unsupported_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(unsupported_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(unsupported_label), STATE_CHARS);
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
  return vbox;
}

void sstv_marks_draw(cairo_t *cr, double rxpos, double cAp, int lsb, double height) {
  if (panel == NULL) { return; }
  double sign = lsb ? -1.0 : 1.0;
  const struct { double hz, r, g, b, w; } m[3] = {
    { 1200.0, 180, 90, 220, 1.5 },    // sync
    { 2300.0, 180, 90, 220, 1.5 },    // white
    { 1500.0, 70, 170, 255, 2.5 },    // black
  };
  cairo_save(cr);
  for (int i = 0; i < 3; i++) {
    double x = rxpos + cAp * sign * m[i].hz;
    cairo_set_source_rgb(cr, m[i].r / 255.0, m[i].g / 255.0, m[i].b / 255.0);
    cairo_set_line_width(cr, m[i].w);
    cairo_move_to(cr, x, 0.0);
    cairo_line_to(cr, x, height);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

//
// program exit with the panel open: put the zoom, pan and filter from before
// SSTV back (values only, the widgets go away with the program) so that
// radio_save_state() stores the user's settings, not SSTV's
//
void sstv_menu_shutdown(void) {
  if (panel == NULL) { return; }
  if (timer_id) { g_source_remove(timer_id); timer_id = 0; }
  sstv_glue_set_rx_enabled(0);
  sstv_glue_filter_restore();
  if (saved_zoom > 0 && receivers > 0 && receiver[0] != NULL) {
    receiver[0]->zoom = saved_zoom;
    receiver[0]->pan = saved_pan;
    saved_zoom = -1;
  }
  t_print("%s: zoom / pan / filter from before SSTV restored\n", __func__);
}

// main menu "SSTV": dock the panel (or close it if it is there)
void sstv_menu(GtkWidget *parent) {
  (void) parent;
  if (panel != NULL) {
    panel_close();
    return;
  }
  if (radio_is_remote) {
    // phase 1: the decoder and the picture are on the server
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(top_window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "SSTV runs on the server for now:\nopen SSTV on the piHPSDR-IF server screen.");
    g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_widget_destroy), dlg);
    gtk_widget_show_all(dlg);
    return;
  }
  panel = build_panel();
  g_object_ref_sink(panel);
  sstv_glue_set_rx_enabled(1);
  sstv_glue_filter_take();
  sstv_glue_fit_filter();     // SSTV filter (1100 - 2400 Hz) right away
  //
  // zoom 26 (~3.7 kHz at 96 kHz): the SSTV band 1100 - 2400 Hz fills the
  // panadapter, as in hpsdr-rs; the zoom from before comes back on close
  //
  if (receivers > 0 && receiver[0] != NULL) {
    saved_zoom = receiver[0]->zoom;
    saved_pan = receiver[0]->pan;
    radio_set_zoom(0, 26);
    center_pan();
  }
  radio_reconfigure();        // spectrum / waterfall to the left, the panel at the right
  status_timer(NULL);
  timer_id = g_timeout_add(200, status_timer, NULL);
}

#endif /* SSTV */
