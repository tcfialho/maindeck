#ifndef WM_STATE_H
#define WM_STATE_H

#include "types.h"

extern struct WindowManager wm;
extern struct wl_display *wm_display;
extern struct wl_registry *wm_registry;
extern struct river_window_manager_v1 *window_manager_v1;
extern struct river_xkb_bindings_v1 *xkb_bindings_v1;
extern struct river_layer_shell_v1 *layer_shell_v1;
extern struct river_libinput_config_v1 *libinput_config_v1;
extern struct wp_cursor_shape_manager_v1 *cursor_shape_manager_v1;

/* ipc_fd e ipc_path são privados de wm-ipc.c (static) — não exportados.
 * Apenas pending_activate_identifier cruza fronteira (lido por wm-layout.c). */
extern char pending_activate_identifier[33];

/* Ação do menu de contexto da barra (minimize/maximize/restore), agendada pelo
 * IPC em wm-ipc.c e aplicada por apply_pending_window_action() em wm-layout.c. */
extern enum WindowAction pending_window_action;
extern char pending_window_action_id[33];

#endif /* WM_STATE_H */
