#ifndef BAR_TASKBAR_H
#define BAR_TASKBAR_H

#include <sys/socket.h>
#include <sys/un.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

extern const struct zwlr_foreign_toplevel_manager_v1_listener bar_mgr_listener;
extern const struct ext_foreign_toplevel_list_v1_listener     bar_ext_list_listener;

void bar_taskbar_activate(int idx);
void bar_taskbar_close(int idx);

/* Ceifador de fantasmas: conjunto de janelas vivas publicado pelo WM. */
void bar_taskbar_set_wm_windows(const char *msg);
void bar_taskbar_prune_ghosts(void);

#endif /* BAR_TASKBAR_H */
