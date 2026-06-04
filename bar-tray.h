#ifndef BAR_TRAY_H
#define BAR_TRAY_H

#include <cairo/cairo.h>

/* Connect to DBus, register as StatusNotifierHost, enumerate existing items.
 * Returns the DBus connection fd to add to poll(), or -1 on failure. */
int  bar_tray_init(void);

/* Drain pending DBus events — call when poll reports the tray fd readable. */
void bar_tray_dispatch(void);

int  bar_tray_count(void);
cairo_surface_t *bar_tray_icon(int idx);
const char      *bar_tray_title(int idx);

/* button: 1 = left (Activate), 3 = right (ContextMenu) */
void bar_tray_click(int idx, int button, int x, int y);

void bar_tray_cleanup(void);

#endif /* BAR_TRAY_H */
