#ifndef BAR_ICONS_H
#define BAR_ICONS_H

#include <cairo/cairo.h>

/* Return a cached icon or queue asynchronous loading and return NULL.
 * Call notify_done on readiness and redraw to replace placeholders.
 * name can be:
 *   "nf:GLYPH"   → rendered via pango as Nerd Font text (returns NULL; caller draws text)
 *   anything else → icon theme lookup via .desktop + PNG/SVG decode
 * Returns cairo_surface_t* or NULL (caller should draw placeholder). */
cairo_surface_t *bar_icon_get(const char *name, int size);

/* Render icon into cairo context at (x, y). If icon is NULL, draws placeholder box. */
void bar_icon_draw(cairo_t *cr, cairo_surface_t *icon, double x, double y, int size);

/* Returns pointer inside `name` (offset +3) for "nf:" cases.
 * Do NOT free; valid as long as the original `name` string exists. */
const char *bar_icon_nf_glyph(const char *name);

void bar_icons_init(void);
int  bar_icons_get_notify_fd(void);
void bar_icons_notify_done(void);

void bar_icons_cleanup(void);
/* Invalidate only completed misses; borrowed successful surfaces stay valid. */
void bar_icons_retry_missing(void);

#endif /* BAR_ICONS_H */

