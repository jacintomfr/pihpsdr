/* Windows (MinGW64) shim for <cairo.h>.
 *
 * ROOT CAUSE FIX for a real crash: MSYS2's cairo (1.18.6) aborts inside its
 * native Windows (DirectWrite) font backend as soon as any text is drawn
 * with the cairo "toy" font API (reproduced with a 10-line test program:
 * cairo_select_font_face() + cairo_show_text() on an image surface -> abort).
 * piHPSDR draws all panadapter/meter/VFO text that way, so the program
 * died the moment a radio was started.
 *
 * All text entry points piHPSDR uses are therefore routed through
 * wrappers that make sure the cairo context carries a fontconfig/FreeType
 * (cairo-ft) font face instead of a toy/DirectWrite one - including the
 * implicit default font of a fresh cairo_t (e.g. the "UDP Packet Loss"
 * message is drawn without selecting a font first). As a bonus, fontconfig
 * also sees the fonts shipped in piHPSDR's fonts/ directory (FreeSans,
 * Roboto, Digital-7, ...), which on Linux get installed system wide.
 * See win_fonts.c.
 */
#pragma once
#include_next <cairo.h>

#ifdef __cplusplus
extern "C" {
#endif
void compat_cairo_select_font_face(cairo_t *cr, const char *family,
                                   cairo_font_slant_t slant, cairo_font_weight_t weight);
void compat_cairo_show_text(cairo_t *cr, const char *utf8);
void compat_cairo_text_path(cairo_t *cr, const char *utf8);
void compat_cairo_text_extents(cairo_t *cr, const char *utf8, cairo_text_extents_t *extents);
void compat_cairo_font_extents(cairo_t *cr, cairo_font_extents_t *extents);
#ifdef __cplusplus
}
#endif

#define cairo_select_font_face(cr,f,s,w) compat_cairo_select_font_face((cr),(f),(s),(w))
#define cairo_show_text(cr,t)            compat_cairo_show_text((cr),(t))
#define cairo_text_path(cr,t)            compat_cairo_text_path((cr),(t))
#define cairo_text_extents(cr,t,e)       compat_cairo_text_extents((cr),(t),(e))
#define cairo_font_extents(cr,e)         compat_cairo_font_extents((cr),(e))
