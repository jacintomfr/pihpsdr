/* On-screen keyboard for piHPSDR-IF on a touch screen, as the hpsdr-rs kiosk's
 * (src/kiosk_keyboard.rs): it appears by itself whenever a text field gets
 * the keyboard focus, the width of the window the field is in, 5 rows of
 * 42 px keys (6 px gaps): 1234567890 Backspace / qwertyuiop / asdfghjkl Enter
 * / Shift zxcvbnm,.- / ?123 space . / : hide, and a symbols layer. Shift is
 * for one character; Backspace repeats while held (after 0.45 s, every 70 ms).
 * The keys cannot take the focus, so the field keeps it.
 *
 * In GTK it is a non-modal popover attached to the focused field (that is
 * what Wayland lets a program place next to its own widget). On by default
 * on Linux (the Pi's touch screen), off on Windows (a real keyboard).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef IFPAN

#include <gtk/gtk.h>
#include <string.h>

#include "osk.h"

#define KEY_H 42
#define GAP 6
#define PAD 6

#ifdef _WIN32
static int osk_enabled = 0;
#else
static int osk_enabled = 1;
#endif

static GtkWidget *pop = NULL;        // the keyboard (popover), attached to `field`
static GtkWidget *field = NULL;      // the GtkEntry being typed into
static GtkWidget *rows_box = NULL;
static int shift = 0, symbols = 0;
static guint bs_timer = 0;

enum { K_CHAR, K_BS, K_ENTER, K_SHIFT, K_SYM, K_HIDE };

typedef struct { const char *label; int kind; double weight; } key_t_;

static void build_rows(void);

static void insert_text(const char *s) {
  if (field == NULL) { return; }
  GtkEditable *e = GTK_EDITABLE(field);
  gint a, b;
  if (gtk_editable_get_selection_bounds(e, &a, &b)) { gtk_editable_delete_text(e, a, b); }
  gint pos = gtk_editable_get_position(e);
  gtk_editable_insert_text(e, s, -1, &pos);
  gtk_editable_set_position(e, pos);
}

static void backspace(void) {
  if (field == NULL) { return; }
  GtkEditable *e = GTK_EDITABLE(field);
  gint a, b;
  if (gtk_editable_get_selection_bounds(e, &a, &b)) {
    gtk_editable_delete_text(e, a, b);
    return;
  }
  gint pos = gtk_editable_get_position(e);
  if (pos > 0) {
    gtk_editable_delete_text(e, pos - 1, pos);
    gtk_editable_set_position(e, pos - 1);
  }
}

static void hide(void) {
  if (bs_timer) { g_source_remove(bs_timer); bs_timer = 0; }
  if (pop != NULL) { gtk_popover_popdown(GTK_POPOVER(pop)); }
}

static void key_clicked(GtkWidget *w, gpointer data) {
  int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "osk-kind"));
  switch (kind) {
  case K_CHAR: {
    const char *s = g_object_get_data(G_OBJECT(w), "osk-text");
    insert_text(s);
    if (shift) { shift = 0; build_rows(); }
    break;
  }
  case K_ENTER:
    if (field != NULL) { g_signal_emit_by_name(field, "activate"); }
    hide();
    break;
  case K_SHIFT:
    shift = !shift;
    build_rows();
    break;
  case K_SYM:
    symbols = !symbols;
    build_rows();
    break;
  case K_HIDE:
    hide();
    break;
  default:
    break;
  }
}

// Backspace: once on press, then repeated while held
static gboolean bs_repeat(gpointer data) {
  backspace();
  bs_timer = g_timeout_add(70, bs_repeat, NULL);
  return G_SOURCE_REMOVE;
}

static gboolean bs_press(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  backspace();
  if (bs_timer) { g_source_remove(bs_timer); }
  bs_timer = g_timeout_add(450, bs_repeat, NULL);
  return FALSE;
}

static gboolean bs_release(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  if (bs_timer) { g_source_remove(bs_timer); bs_timer = 0; }
  return FALSE;
}

static void key_css(GtkWidget *b, int active) {
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_data(css, active
                                  ? "button { background-image: none; background-color: rgb(200, 110, 30); color: rgb(235, 235, 235);"
                                    " font-size: 20px; border-radius: 5px; padding: 0; }"
                                  : "button { background-image: none; background-color: rgb(52, 52, 52); color: rgb(235, 235, 235);"
                                    " font-size: 20px; border-radius: 5px; padding: 0; }"
                                    " button:active { background-color: rgb(110, 110, 110); }",
                                  -1, NULL);
  gtk_style_context_add_provider(gtk_widget_get_style_context(b), GTK_STYLE_PROVIDER(css),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
}

// one row of keys, `chars` one key each, with optional special keys before / after
static void add_row(int width, const key_t_ *pre, int npre, const char *chars, const key_t_ *post, int npost) {
  key_t_ k[24];
  int n = 0;
  static char cbuf[24][8];
  for (int i = 0; i < npre; i++) { k[n++] = pre[i]; }
  for (int i = 0; chars && chars[i] && n < 24; i++) {
    char c = chars[i];
    if (shift && c >= 'a' && c <= 'z') { c = (char)(c - 'a' + 'A'); }
    snprintf(cbuf[n], sizeof(cbuf[n]), "%c", c);
    k[n].label = cbuf[n];
    k[n].kind = K_CHAR;
    k[n].weight = 1.0;
    n++;
  }
  for (int i = 0; i < npost; i++) { k[n++] = post[i]; }
  double total = 0;
  for (int i = 0; i < n; i++) { total += k[i].weight; }
  double unit = (width - 2.0 * PAD - GAP * (n - 1)) / total;
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, GAP);
  for (int i = 0; i < n; i++) {
    GtkWidget *b = gtk_button_new_with_label(k[i].label);
    gtk_widget_set_can_focus(b, FALSE);          // the field keeps the focus
    gtk_widget_set_size_request(b, (int)(unit * k[i].weight), KEY_H);
    key_css(b, (k[i].kind == K_SHIFT && shift) || (k[i].kind == K_SYM && symbols));
    g_object_set_data(G_OBJECT(b), "osk-kind", GINT_TO_POINTER(k[i].kind));
    if (k[i].kind == K_CHAR) {
      // "space" is shown, a blank is typed
      g_object_set_data_full(G_OBJECT(b), "osk-text", g_strdup(strcmp(k[i].label, "space") == 0 ? " " : k[i].label), g_free);
    }
    if (k[i].kind == K_BS) {
      g_signal_connect(b, "button-press-event", G_CALLBACK(bs_press), NULL);
      g_signal_connect(b, "button-release-event", G_CALLBACK(bs_release), NULL);
    } else {
      g_signal_connect(b, "clicked", G_CALLBACK(key_clicked), NULL);
    }
    gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(rows_box), row, FALSE, FALSE, 0);
}

static void build_rows(void) {
  if (rows_box == NULL || field == NULL) { return; }
  GList *children = gtk_container_get_children(GTK_CONTAINER(rows_box));
  for (GList *l = children; l != NULL; l = l->next) { gtk_widget_destroy(GTK_WIDGET(l->data)); }
  g_list_free(children);
  GtkWidget *top = gtk_widget_get_toplevel(field);
  int width = gtk_widget_get_allocated_width(top) - 24;
  if (width < 360) { width = 360; }
  static const key_t_ bs[] = { { "⌫", K_BS, 1.4 } };
  static const key_t_ enter_l[] = { { "↵", K_ENTER, 1.6 } };
  static const key_t_ enter_s[] = { { "↵", K_ENTER, 1.4 } };
  static const key_t_ shift_k[] = { { "⇧", K_SHIFT, 1.4 } };
  if (symbols) {
    static const key_t_ last[] = { { "ABC", K_SYM, 1.6 }, { "space", K_CHAR, 5.0 }, { ".", K_CHAR, 1.0 },
                                   { "/", K_CHAR, 1.0 }, { ":", K_CHAR, 1.0 }, { "hide", K_HIDE, 1.6 } };
    add_row(width, NULL, 0, "!@#$%^&*()", bs, 1);
    add_row(width, NULL, 0, "-_=+[]{}\\|", NULL, 0);
    add_row(width, NULL, 0, ";:'\"<>?/~", enter_s, 1);
    add_row(width, NULL, 0, "`.,@#_-+*/", NULL, 0);
    add_row(width, last, 6, NULL, NULL, 0);
  } else {
    static const key_t_ last[] = { { "?123", K_SYM, 1.6 }, { "space", K_CHAR, 5.0 }, { ".", K_CHAR, 1.0 },
                                   { "/", K_CHAR, 1.0 }, { ":", K_CHAR, 1.0 }, { "hide", K_HIDE, 1.6 } };
    add_row(width, NULL, 0, "1234567890", bs, 1);
    add_row(width, NULL, 0, "qwertyuiop", NULL, 0);
    add_row(width, NULL, 0, "asdfghjkl", enter_l, 1);
    add_row(width, shift_k, 1, "zxcvbnm,.-", NULL, 0);
    add_row(width, last, 6, NULL, NULL, 0);
  }
  gtk_widget_show_all(rows_box);
}

static gboolean pop_bg_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
  cairo_set_source_rgb(cr, 24 / 255.0, 24 / 255.0, 24 / 255.0);
  cairo_paint(cr);
  return FALSE;
}

static void field_gone(gpointer data, GObject *where) {
  // the field was destroyed (its menu closed): the keyboard went with it
  if ((GObject *) field == where) {
    field = NULL;
    pop = NULL;
    rows_box = NULL;
    if (bs_timer) { g_source_remove(bs_timer); bs_timer = 0; }
  }
}

static gboolean field_focus_out(GtkWidget *w, GdkEvent *ev, gpointer data) {
  if (w == field) { hide(); }
  return FALSE;
}

static void show_for(GtkWidget *entry) {
  if (entry != field) {
    if (pop != NULL) { gtk_widget_destroy(pop); }
    if (field != NULL) { g_object_weak_unref(G_OBJECT(field), field_gone, NULL); }
    field = entry;
    g_object_weak_ref(G_OBJECT(field), field_gone, NULL);
    g_signal_connect(field, "focus-out-event", G_CALLBACK(field_focus_out), NULL);
    pop = gtk_popover_new(field);
    gtk_popover_set_modal(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);   // GTK flips it above if needed
    GtkWidget *ev = gtk_event_box_new();
    g_signal_connect(ev, "draw", G_CALLBACK(pop_bg_draw), NULL);
    rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, GAP);
    gtk_container_set_border_width(GTK_CONTAINER(rows_box), PAD);
    gtk_container_add(GTK_CONTAINER(ev), rows_box);
    gtk_container_add(GTK_CONTAINER(pop), ev);
    gtk_widget_show_all(ev);
  }
  shift = 0;
  symbols = 0;
  build_rows();
  gtk_popover_popup(GTK_POPOVER(pop));
}

static gboolean focus_hook(GSignalInvocationHint *ihint, guint n, const GValue *params, gpointer data) {
  if (!osk_enabled || n < 1) { return TRUE; }
  GObject *o = g_value_get_object(&params[0]);
  if (o != NULL && GTK_IS_ENTRY(o) && gtk_editable_get_editable(GTK_EDITABLE(o))) {
    show_for(GTK_WIDGET(o));
  }
  return TRUE;                // keep the hook
}

void osk_init(void) {
  static int done = 0;
  if (done) { return; }
  done = 1;
  g_type_class_unref(g_type_class_ref(GTK_TYPE_ENTRY));
  guint sig = g_signal_lookup("focus-in-event", GTK_TYPE_WIDGET);
  g_signal_add_emission_hook(sig, 0, focus_hook, NULL, NULL);
}

void osk_set_enabled(int on) {
  osk_enabled = on != 0;
  if (!osk_enabled) { hide(); }
}

int osk_get_enabled(void) {
  return osk_enabled;
}

//
// Touch style: the whole program's buttons as the keyboard's keys (dark,
// thin border, 5 px rounded corners, lighter while pressed, orange when a
// toggle is on) and check boxes as big squares with an X (as the hpsdr-rs
// kiosk). One CSS for the screen, just below piHPSDR's own (css.c, priority
// APPLICATION): its named buttons keep their colours (red / orange / green /
// ... state buttons, menu, hide), and so do the widgets with a style of their
// own (the green Auto, the keyboard keys, the RADE check boxes). Off: the
// look of before. On by default on Linux (the Pi's touch screen), off on
// Windows; saved as ifpan.touch_style.
//
#ifdef _WIN32
int touch_style = 0;
#else
int touch_style = 1;
#endif
static GtkCssProvider *touch_css = NULL;

static const char *touch_rules =
  "button { background-image: none; background-color: rgb(52, 52, 52); color: rgb(235, 235, 235);"
  " border: 1px solid rgb(95, 95, 95); border-radius: 5px; box-shadow: none; text-shadow: none; }\n"
  "button:hover { background-color: rgb(64, 64, 64); }\n"
  "button:active { background-color: rgb(110, 110, 110); }\n"
  "button:checked { background-color: rgb(200, 110, 30); color: rgb(235, 235, 235); }\n"
  "button:disabled { background-color: rgb(40, 40, 40); color: rgb(120, 120, 120); }\n"
  "checkbutton check { min-width: 24px; min-height: 24px; -gtk-icon-source: none; background-image: none;"
  " background-color: rgb(25, 25, 25); border: 2px solid rgb(170, 170, 170); border-radius: 3px; }\n"
  "checkbutton check:checked { -gtk-icon-source: -gtk-icontheme('window-close-symbolic'); color: rgb(255, 255, 255); }\n";

void touch_style_set(int on) {
  touch_style = on != 0;
  GdkScreen *screen = gdk_screen_get_default();
  if (screen == NULL) { return; }
  if (touch_css == NULL) {
    touch_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(touch_css, touch_rules, -1, NULL);
  }
  gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(touch_css));
  if (touch_style) {
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(touch_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 1);
  }
}

#endif /* IFPAN */
