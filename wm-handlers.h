#ifndef WM_HANDLERS_H
#define WM_HANDLERS_H

#include "types.h"

void output_maybe_destroy(struct Output *output);
void window_maybe_destroy(struct Window *window);
void wm_init(void);

extern const struct river_window_manager_v1_listener wm_listener;
extern const struct wl_registry_listener registry_listener;

#endif /* WM_HANDLERS_H */
