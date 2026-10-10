/* Copyright (C)
* 2026 - piHPSDR Windows (MSYS2 MinGW64) port
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*/

/*
 * Font handling for the Windows build (see cairo.h shim for the WHY):
 *
 *  1. cairo_select_font_face() is served by fontconfig + cairo-ft instead
 *     of cairo's DirectWrite backend, which aborts in MSYS2's cairo 1.18.6.
 *  2. If a fonts/ folder exists next to the .exe, it is registered with
 *     fontconfig, so fonts can be shipped without installing anything into
 *     Windows (piHPSDR's css.c font names - Open Sans, FreeSans, Roboto, ...
 *     - otherwise fall back to the Windows fonts via fontconfig).
 *  3. pango (GTK widgets) is switched to its fontconfig backend
 *     (PANGOCAIRO_BACKEND=fc) before main() runs, so it shares the same
 *     fontconfig configuration and avoids the same cairo abort.
 */

#ifdef _WIN32

#include <windows.h>
#include <cairo.h>
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef cairo_select_font_face
#undef cairo_show_text
#undef cairo_text_path
#undef cairo_text_extents
#undef cairo_font_extents

static GMutex font_mutex;
static GHashTable *font_cache = NULL;   /* "family|slant|weight" -> cairo_font_face_t* */

/*
 * Register <exe dir>/fonts with fontconfig. Runs before main(), i.e. before
 * pango or cairo look at fonts for the first time.
 */
__attribute__((constructor))
static void win_fonts_init(void) {
  char exe[MAX_PATH];
  /* see 3. above; a value set by the user is kept */
  if (GetEnvironmentVariableA("PANGOCAIRO_BACKEND", NULL, 0) == 0) {
    SetEnvironmentVariableA("PANGOCAIRO_BACKEND", "fc");
    _putenv("PANGOCAIRO_BACKEND=fc");
  }
  DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
  if (n == 0 || n >= sizeof(exe)) { return; }
  char *slash = strrchr(exe, '\\');
  if (!slash) { return; }
  *slash = '\0';
  char dir[MAX_PATH + 16];
  snprintf(dir, sizeof(dir), "%s\\fonts", exe);
  if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) { return; }
  if (!FcInit()) { return; }
  /* FcConfigAppFontAddDir scans the directory recursively */
  FcConfigAppFontAddDir(NULL, (const FcChar8 *) dir);
}

void compat_cairo_select_font_face(cairo_t *cr, const char *family,
                                   cairo_font_slant_t slant, cairo_font_weight_t weight) {
  char key[256];
  cairo_font_face_t *face;
  if (!family) { family = "sans-serif"; }
  snprintf(key, sizeof(key), "%s|%d|%d", family, (int) slant, (int) weight);
  g_mutex_lock(&font_mutex);
  if (!font_cache) {
    font_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }
  face = g_hash_table_lookup(font_cache, key);
  if (!face) {
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *) family);
    /* names like "Roboto-Bold"/"FreeSansBold" are also full font names */
    FcPatternAddString(pat, FC_FULLNAME, (const FcChar8 *) family);
    FcPatternAddInteger(pat, FC_SLANT, slant == CAIRO_FONT_SLANT_ITALIC ? FC_SLANT_ITALIC :
                        slant == CAIRO_FONT_SLANT_OBLIQUE ? FC_SLANT_OBLIQUE : FC_SLANT_ROMAN);
    FcPatternAddInteger(pat, FC_WEIGHT, weight == CAIRO_FONT_WEIGHT_BOLD ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    face = cairo_ft_font_face_create_for_pattern(match ? match : pat);
    if (match) { FcPatternDestroy(match); }
    FcPatternDestroy(pat);
    if (cairo_font_face_status(face) == CAIRO_STATUS_SUCCESS) {
      g_hash_table_insert(font_cache, g_strdup(key), face);    /* cache keeps one reference */
    }
  }
  g_mutex_unlock(&font_mutex);
  cairo_set_font_face(cr, face);   /* cairo_t takes its own reference */
}

/*
 * If the context still has a toy font face (the default "sans-serif" of a
 * new cairo_t, or one set via cairo_toy_font_face_create()), swap in the
 * equivalent FreeType face before cairo resolves it via DirectWrite.
 */
static void ensure_ft_face(cairo_t *cr) {
  cairo_font_face_t *cur = cairo_get_font_face(cr);
  if (cur && cairo_font_face_get_type(cur) == CAIRO_FONT_TYPE_TOY) {
    compat_cairo_select_font_face(cr, cairo_toy_font_face_get_family(cur),
                                  cairo_toy_font_face_get_slant(cur),
                                  cairo_toy_font_face_get_weight(cur));
  }
}

void compat_cairo_show_text(cairo_t *cr, const char *utf8) {
  ensure_ft_face(cr);
  cairo_show_text(cr, utf8);
}

void compat_cairo_text_path(cairo_t *cr, const char *utf8) {
  ensure_ft_face(cr);
  cairo_text_path(cr, utf8);
}

void compat_cairo_text_extents(cairo_t *cr, const char *utf8, cairo_text_extents_t *extents) {
  ensure_ft_face(cr);
  cairo_text_extents(cr, utf8, extents);
}

void compat_cairo_font_extents(cairo_t *cr, cairo_font_extents_t *extents) {
  ensure_ft_face(cr);
  cairo_font_extents(cr, extents);
}

#endif /* _WIN32 */
