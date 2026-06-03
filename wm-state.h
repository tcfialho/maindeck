#ifndef WM_STATE_H
#define WM_STATE_H

#include "types.h"

extern struct WindowManager wm;
extern struct river_window_manager_v1 *window_manager_v1;
extern struct river_xkb_bindings_v1 *xkb_bindings_v1;
extern struct river_layer_shell_v1 *layer_shell_v1;
extern struct river_libinput_config_v1 *libinput_config_v1;

/* ipc_fd e ipc_path são privados de wm-ipc.c (static) — não exportados.
 * Apenas pending_activate_identifier cruza fronteira (lido por wm-layout.c). */
extern char pending_activate_identifier[33];

#endif /* WM_STATE_H */
