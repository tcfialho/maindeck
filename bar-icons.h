#ifndef BAR_ICONS_H
#define BAR_ICONS_H

#include <cairo/cairo.h>

/* Load (or return cached) icon surface for the given name.
 * name can be:
 *   "nf:GLYPH"   → rendered via pango as Nerd Font text (returns NULL; caller draws text)
 *   anything else → icon theme lookup via .desktop + gdk-pixbuf decode
 * Returns cairo_surface_t* or NULL (caller should draw placeholder). */
cairo_surface_t *bar_icon_get(const char *name, int size);

/* Render icon into cairo context at (x, y). If icon is NULL, draws placeholder box. */
void bar_icon_draw(cairo_t *cr, cairo_surface_t *icon, double x, double y, int size);

/* Free the NF glyph string if the icon name is a nf: prefix icon.
 * Returns the glyph string (caller owns it) or NULL. */
char *bar_icon_nf_glyph(const char *name);

void bar_icons_cleanup(void);

#endif /* BAR_ICONS_H */
