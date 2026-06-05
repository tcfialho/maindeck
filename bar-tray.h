#ifndef BAR_TRAY_H
#define BAR_TRAY_H

#include <stdint.h>
#include <cairo/cairo.h>

/* Connect to DBus, register as StatusNotifierHost, enumerate existing items.
 * Returns the DBus connection fd to add to poll(), or -1 on failure. */
int  bar_tray_init(void);

/* Drain pending DBus events — call when poll reports the tray fd readable. */
void bar_tray_dispatch(void);

int  bar_tray_count(void);
cairo_surface_t *bar_tray_icon(int idx);
const char      *bar_tray_title(int idx);

/* button: 1 = left (Activate), 3 = right (open dbusmenu popup) */
void bar_tray_click(int idx, int button, int x, int y);

/* Open a dbusmenu context menu popup for item idx.
 * icon_x, icon_w: screen coords of the icon (for positioner anchor).
 * serial: wl_pointer serial from the button press (for xdg_popup grab). */
void bar_tray_open_menu(int idx, int icon_x, int icon_w, uint32_t serial);

/* Returns which menu row index is at pixel y (-1 = none / separator). */
int bar_tray_menu_row_at(int y);

/* Activate (click) menu row. time = wl_pointer time for dbusmenu Event. */
void bar_tray_menu_activate(int row, uint32_t time);

/* Re-render menu after hover change. */
void bar_tray_menu_rerend(void);

/* Close and destroy the popup if open. */
void bar_tray_menu_close(void);

void bar_tray_cleanup(void);

#endif /* BAR_TRAY_H */
