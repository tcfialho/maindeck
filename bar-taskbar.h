#ifndef BAR_TASKBAR_H
#define BAR_TASKBAR_H

#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

extern const struct zwlr_foreign_toplevel_manager_v1_listener bar_mgr_listener;
extern const struct ext_foreign_toplevel_list_v1_listener     bar_ext_list_listener;

void bar_taskbar_activate(int idx);
void bar_taskbar_close(int idx);
/* Fecha a janela pelo identifier estável (usado pelo menu de contexto). */
void bar_taskbar_close_by_id(const char *id);

/* Envia "<verb> <id>" ao socket IPC do WM (activate/minimize/maximize/restore). */
void bar_taskbar_send_wm(const char *verb, const char *id);

/* Abre o menu de contexto (botão direito) da janela idx, ancorado em icon_x/
 * icon_w (da hit area); serial = wl_pointer serial do clique (para o grab). */
void bar_taskbar_open_menu(int idx, int icon_x, int icon_w, uint32_t serial);

/* Ceifador de fantasmas: conjunto de janelas vivas publicado pelo WM. */
void bar_taskbar_set_wm_windows(const char *msg);
void bar_taskbar_prune_ghosts(void);

#endif /* BAR_TASKBAR_H */
