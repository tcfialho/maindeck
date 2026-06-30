#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#include "bar-state.h"
#include "bar-taskbar.h"
#include "bar-surface.h"
#include "bar-icons.h"
#include "bar-log.h"
#include "bar-game-mode.h"

static void bar_update_render_suppressed(void) {
    struct BarState *bar = &g_bar;
    /* WM notify takes precedence — don't override with zwlr state */
    if (bar->wm_fullscreen) return;
    bool any_active_fullscreen = false;
    for (int i = 0; i < bar->toplevel_n; i++) {
        struct BarToplevel *tl = bar->toplevels[i];
        if (tl && !tl->has_parent && tl->activated && tl->fullscreen && !tl->minimized) {
            any_active_fullscreen = true;
            break;
        }
    }

    if (any_active_fullscreen != bar->render_suppressed) {
        LOG_INFO("bar: render_suppressed changed from %d to %d", bar->render_suppressed, any_active_fullscreen);
        bar->render_suppressed = any_active_fullscreen;
        bar_game_mode_apply(any_active_fullscreen);
        if (!bar->render_suppressed) {
            bar->dirty_deferred = false;
            bar_surface_restore();
        } else {
            bar->dirty = false;
            bar->dirty_deferred = true;
            bar_surface_destroy();
        }
    }
}

/* ------------------------------------------------------------------ */
/* zwlr handlers                                                        */
/* ------------------------------------------------------------------ */
static uint64_t g_pending_seq = 0;

static bool is_zwlr_pending(void *data) {
    struct BarState *bar = &g_bar;
    return (data >= (void*)&bar->zwlr_pending[0] &&
            data < (void*)&bar->zwlr_pending[BAR_MAX_PENDING]);
}

static bool is_ext_pending(void *data) {
    struct BarState *bar = &g_bar;
    return (data >= (void*)&bar->ext_pending[0] &&
            data < (void*)&bar->ext_pending[BAR_MAX_PENDING]);
}

static void try_match_pending(void);

static void tl_title(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h;
    if (is_zwlr_pending(data)) {
        PendingEntry *pe = data;
        snprintf(pe->title, sizeof(pe->title), "%s", s ? s : "");
    } else {
        struct BarToplevel *tl = data;
        snprintf(tl->title, sizeof(tl->title), "%s", s ? s : "");
        bar_request_redraw_flags(&g_bar, BAR_DIRTY_TASKBAR);
    }
}

static void tl_app_id(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h;
    if (is_zwlr_pending(data)) {
        PendingEntry *pe = data;
        snprintf(pe->app_id, sizeof(pe->app_id), "%s", s ? s : "");
    } else {
        struct BarToplevel *tl = data;
        snprintf(tl->app_id, sizeof(tl->app_id), "%s", s ? s : "");
        if (!tl->icon_surface && s && s[0]) {
            tl->icon_surface = bar_icon_get(s, 18);
        }
        bar_request_redraw_flags(&g_bar, BAR_DIRTY_TASKBAR);
    }
}

static void tl_oe(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d; (void)h; (void)o; }

static void tl_ol(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d; (void)h; (void)o; }

static void tl_state(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st) {
    (void)h;
    if (is_zwlr_pending(data)) {
        PendingEntry *pe = data;
        bool act = false, fs = false;
        uint32_t *s;
        wl_array_for_each(s, st) {
            if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) act = true;
            if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) fs = true;
        }
        pe->activated = act;
        pe->fullscreen = fs;
        return;
    }
    struct BarToplevel *tl = data;
    bool act = false, fs = false;
    uint32_t *s;
    wl_array_for_each(s, st) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) act = true;
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) fs = true;
    }
    tl->activated = act;
    tl->fullscreen = fs;
    bar_request_redraw_flags(&g_bar, BAR_DIRTY_TASKBAR);
    if (tl->app_id[0])
        bar_update_render_suppressed();
}

static void taskbar_remove_entry(int idx) {
    struct BarState *bar = &g_bar;
    struct BarToplevel *tl = bar->toplevels[idx];
    if (tl->layout) {
        g_object_unref(tl->layout);
    }
    if (tl->zwlr_handle) {
        zwlr_foreign_toplevel_handle_v1_destroy(tl->zwlr_handle);
    }
    if (tl->ext_handle) {
        ext_foreign_toplevel_handle_v1_destroy(tl->ext_handle);
    }
    free(tl);
    int rem = bar->toplevel_n - idx - 1;
    if (rem > 0) {
        memmove(&bar->toplevels[idx], &bar->toplevels[idx+1],
                (size_t)rem * sizeof(bar->toplevels[0]));
    }
    bar->toplevel_n--;
    bar->toplevels[bar->toplevel_n] = NULL;
}

static void tl_done(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h;
    if (is_zwlr_pending(data)) {
        PendingEntry *pe = data;
        pe->done = true;
        try_match_pending();
    } else {
        bar_update_render_suppressed();
        bar_request_redraw_flags(&g_bar, BAR_DIRTY_TASKBAR);
    }
}

static void tl_closed(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    struct BarState *bar = &g_bar;
    if (is_zwlr_pending(data)) {
        PendingEntry *pe = data;
        pe->in_use = false;
        zwlr_foreign_toplevel_handle_v1_destroy(h);
    } else {
        struct BarToplevel *tl = data;
        int idx = -1;
        for (int i = 0; i < bar->toplevel_n; i++) {
            if (bar->toplevels[i] == tl) { idx = i; break; }
        }
        if (idx >= 0) {
            taskbar_remove_entry(idx);
        } else {
            zwlr_foreign_toplevel_handle_v1_destroy(h);
        }
        bar_request_redraw_flags(&g_bar, BAR_DIRTY_TASKBAR);
        bar_update_render_suppressed();
    }
    LOG_INFO("taskbar: toplevel closed, remaining=%d", g_bar.toplevel_n);
}

static void tl_parent(void *d,
    struct zwlr_foreign_toplevel_handle_v1 *h,
    struct zwlr_foreign_toplevel_handle_v1 *p) {
    (void)h;
    if (is_zwlr_pending(d)) return;
    struct BarToplevel *tl = d;
    bool has_parent = p != NULL;
    if (tl && tl->has_parent != has_parent) {
        tl->has_parent = has_parent;
        bar_request_redraw_flags(&g_bar, BAR_DIRTY_TASKBAR);
        bar_update_render_suppressed();
    }
}

static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
    .title        = tl_title,
    .app_id       = tl_app_id,
    .output_enter = tl_oe,
    .output_leave = tl_ol,
    .state        = tl_state,
    .done         = tl_done,
    .closed       = tl_closed,
    .parent       = tl_parent,
};

/* ------------------------------------------------------------------ */
/* zwlr manager: toplevel / finished                                    */
/* ------------------------------------------------------------------ */

static void mgr_toplevel(void *data,
    struct zwlr_foreign_toplevel_manager_v1 *mgr,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)data; (void)mgr;
    struct BarState *bar = &g_bar;
    
    int idx = -1;
    for (int i = 0; i < BAR_MAX_PENDING; i++) {
        if (!bar->zwlr_pending[i].in_use) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        LOG_WARN("taskbar: pending queue full, destroying new toplevel");
        zwlr_foreign_toplevel_handle_v1_destroy(h);
        return;
    }
    
    PendingEntry *pe = &bar->zwlr_pending[idx];
    pe->in_use = true;
    pe->done = false;
    pe->handle = h;
    pe->app_id[0] = '\0';
    pe->title[0] = '\0';
    pe->identifier[0] = '\0';
    pe->activated = false;
    pe->fullscreen = false;
    pe->seq_num = ++g_pending_seq;
    
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, pe);
}

static void mgr_finished(void *d,
    struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }

const struct zwlr_foreign_toplevel_manager_v1_listener bar_mgr_listener = {
    .toplevel = mgr_toplevel,
    .finished = mgr_finished,
};

/* ------------------------------------------------------------------ */
/* ext-foreign-toplevel handlers                                        */
/* ------------------------------------------------------------------ */static void ext_identifier(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *id) {
    (void)handle;
    if (is_ext_pending(data)) {
        PendingEntry *pe = data;
        snprintf(pe->identifier, sizeof(pe->identifier), "%s", id ? id : "");
    }
}

static void ext_title(void *data,
    struct ext_foreign_toplevel_handle_v1 *h, const char *t) {
    (void)h;
    if (is_ext_pending(data)) {
        PendingEntry *pe = data;
        snprintf(pe->title, sizeof(pe->title), "%s", t ? t : "");
    }
}

static void ext_app_id(void *data,
    struct ext_foreign_toplevel_handle_v1 *h, const char *a) {
    (void)h;
    if (is_ext_pending(data)) {
        PendingEntry *pe = data;
        snprintf(pe->app_id, sizeof(pe->app_id), "%s", a ? a : "");
    }
}

static void try_match_pending(void) {
    struct BarState *bar = &g_bar;
    bool tried[BAR_MAX_PENDING] = {0};
    while (1) {
        PendingEntry *zwlr_pe = NULL;
        int zwlr_idx = -1;
        for (int i = 0; i < BAR_MAX_PENDING; i++) {
            if (tried[i]) continue;
            PendingEntry *pe = &bar->zwlr_pending[i];
            if (!pe->in_use || !pe->done) continue;
            if (pe->app_id[0] == '\0' && pe->title[0] == '\0') continue;
            if (!zwlr_pe || pe->seq_num < zwlr_pe->seq_num) {
                zwlr_pe = pe;
                zwlr_idx = i;
            }
        }
        if (!zwlr_pe) break;

        PendingEntry *best_ext_pe = NULL;
        for (int j = 0; j < BAR_MAX_PENDING; j++) {
            PendingEntry *ext_pe = &bar->ext_pending[j];
            if (!ext_pe->in_use || !ext_pe->done) continue;

            if (strcmp(zwlr_pe->app_id, ext_pe->app_id) == 0 &&
                strcmp(zwlr_pe->title, ext_pe->title) == 0) {
                if (!best_ext_pe || ext_pe->seq_num < best_ext_pe->seq_num) {
                    best_ext_pe = ext_pe;
                }
            }
        }

        if (best_ext_pe) {
            if (bar->toplevel_n >= BAR_MAX_TOPLEVELS) {
                LOG_WARN("taskbar: too many toplevels, cannot add matched entry");
                zwlr_pe->in_use = false;
                best_ext_pe->in_use = false;
                continue;
            }

            struct BarToplevel *tl = calloc(1, sizeof(*tl));
            if (!tl) {
                LOG_ERR("taskbar: calloc failed for matched toplevel");
                zwlr_pe->in_use = false;
                best_ext_pe->in_use = false;
                continue;
            }

            tl->zwlr_handle = zwlr_pe->handle;
            tl->ext_handle = best_ext_pe->handle;
            snprintf(tl->identifier, sizeof(tl->identifier), "%s", best_ext_pe->identifier);
            snprintf(tl->title, sizeof(tl->title), "%s", zwlr_pe->title);
            snprintf(tl->app_id, sizeof(tl->app_id), "%s", zwlr_pe->app_id);
            tl->activated = zwlr_pe->activated;
            tl->fullscreen = zwlr_pe->fullscreen;
            clock_gettime(CLOCK_MONOTONIC, &tl->created);

            if (tl->app_id[0]) {
                tl->icon_surface = bar_icon_get(tl->app_id, 18);
            }

            bar->toplevels[bar->toplevel_n] = tl;
            bar->toplevel_n++;

            zwlr_foreign_toplevel_handle_v1_set_user_data(tl->zwlr_handle, tl);
            ext_foreign_toplevel_handle_v1_set_user_data(tl->ext_handle, tl);

            zwlr_pe->in_use = false;
            best_ext_pe->in_use = false;

            bar_request_redraw_flags(bar, BAR_DIRTY_TASKBAR);
            bar_update_render_suppressed();

            LOG_INFO("taskbar: matched app_id=%s title=%s identifier=%s act=%d fs=%d",
                tl->app_id, tl->title, tl->identifier, tl->activated, tl->fullscreen);
        } else {
            tried[zwlr_idx] = true;
        }
    }
}

static void ext_done(void *data,
    struct ext_foreign_toplevel_handle_v1 *h) {
    (void)h;
    if (is_ext_pending(data)) {
        PendingEntry *pe = data;
        pe->done = true;
        try_match_pending();
    }
}

static void ext_closed(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle) {
    struct BarState *bar = &g_bar;
    if (is_ext_pending(data)) {
        PendingEntry *pe = data;
        pe->in_use = false;
        ext_foreign_toplevel_handle_v1_destroy(handle);
    } else {
        struct BarToplevel *tl = NULL;
        for (int i = 0; i < bar->toplevel_n; i++) {
            if (bar->toplevels[i]->ext_handle == handle) {
                tl = bar->toplevels[i];
                break;
            }
        }
        if (tl) {
            tl->ext_handle = NULL;
            tl->identifier[0] = '\0';
        }
        ext_foreign_toplevel_handle_v1_destroy(handle);
    }
}

static const struct ext_foreign_toplevel_handle_v1_listener ext_handle_listener = {
    .identifier = ext_identifier,
    .title      = ext_title,
    .app_id     = ext_app_id,
    .done       = ext_done,
    .closed     = ext_closed,
};

static void ext_list_toplevel(void *data,
    struct ext_foreign_toplevel_list_v1 *list,
    struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)data; (void)list;
    struct BarState *bar = &g_bar;

    int idx = -1;
    for (int i = 0; i < BAR_MAX_PENDING; i++) {
        if (!bar->ext_pending[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        LOG_WARN("taskbar: pending queue full, destroying new ext toplevel");
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }

    PendingEntry *pe = &bar->ext_pending[idx];
    pe->in_use = true;
    pe->done = false;
    pe->handle = handle;
    pe->app_id[0] = '\0';
    pe->title[0] = '\0';
    pe->identifier[0] = '\0';
    pe->activated = false;
    pe->fullscreen = false;
    pe->seq_num = ++g_pending_seq;

    ext_foreign_toplevel_handle_v1_add_listener(handle, &ext_handle_listener, pe);
}

static void ext_list_finished(void *d,
    struct ext_foreign_toplevel_list_v1 *l) { (void)d; (void)l; }

const struct ext_foreign_toplevel_list_v1_listener bar_ext_list_listener = {
    .toplevel = ext_list_toplevel,
    .finished = ext_list_finished,
};

/* ------------------------------------------------------------------ */
/* IPC: activate via maindeck-wm socket                                 */
/* ------------------------------------------------------------------ */

/* Envia "<verb> <id>" ao socket IPC do WM (maindeck-wm.sock). Usado por
 * activate (clique esquerdo) e pelos comandos do menu de contexto
 * (minimize/maximize/restore). id deve ser não-vazio. */
void bar_taskbar_send_wm(const char *verb, const char *id) {
    struct BarState *bar = &g_bar;
    if (!id || !id[0]) {
        LOG_WARN("taskbar: %s sem identifier, ignorado", verb);
        return;
    }
    if (bar->ipc_sock < 0) {
        LOG_WARN("taskbar: IPC socket not connected");
        return;
    }

    char msg[64];
    int  len = snprintf(msg, sizeof(msg), "%s %s", verb, id);
    if (len < 0 || (size_t)len >= sizeof(msg)) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", bar->ipc_path);

    ssize_t sent = sendto(bar->ipc_sock, msg, (size_t)len, 0,
                          (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0) {
        LOG_WARN("taskbar: sendto ipc failed: %m");
    } else {
        LOG_INFO("taskbar: sent '%s' to %s", msg, bar->ipc_path);
    }
}

void bar_taskbar_activate(int idx) {
    struct BarState *bar = &g_bar;
    if (idx < 0 || idx >= bar->toplevel_n) return;

    struct BarToplevel *tl = bar->toplevels[idx];
    if (!tl || !tl->identifier[0]) {
        LOG_WARN("taskbar: no identifier for idx=%d, cannot activate", idx);
        return;
    }
    bar_taskbar_send_wm("activate", tl->identifier);
}
 
void bar_taskbar_close(int idx) {
    struct BarState *bar = &g_bar;
    if (idx < 0 || idx >= bar->toplevel_n) return;
    struct BarToplevel *tl = bar->toplevels[idx];
    if (tl && tl->zwlr_handle) {
        zwlr_foreign_toplevel_handle_v1_close(tl->zwlr_handle);
    }
}

/* Fecha a janela pelo identifier (estável), não por índice. Necessário para o
 * menu de contexto: enquanto o popup está aberto, o dispatch da barra continua,
 * então o array toplevels[] pode ser remanejado (memmove em tl_closed/ghost
 * prune) e um índice capturado na abertura ficaria obsoleto. */
void bar_taskbar_close_by_id(const char *id) {
    struct BarState *bar = &g_bar;
    if (!id || !id[0]) return;
    for (int i = 0; i < bar->toplevel_n; i++) {
        struct BarToplevel *tl = bar->toplevels[i];
        if (tl && tl->identifier[0] && strcmp(tl->identifier, id) == 0) {
            if (tl->zwlr_handle) {
                zwlr_foreign_toplevel_handle_v1_close(tl->zwlr_handle);
            }
            return;
        }
    }
    LOG_WARN("taskbar: close_by_id id desconhecido=%s (no-op)", id);
}

/* ------------------------------------------------------------------ */
/* Ceifador de fantasmas                                                */
/*                                                                      */
/* O compositor pode vazar handles de foreign-toplevel para janelas que */
/* morreram antes do primeiro map (ex.: o diálogo zenity do protonfixes */
/* ao abrir jogos na Steam). A entrada fica na taskbar para sempre: não */
/* é clicável (o WM não conhece o identifier) e acumula a cada launch.  */
/* O WM publica o conjunto de identifiers vivos a cada manage; qualquer */
/* entrada pareada ausente desse conjunto é fantasma e é removida.      */
/* ------------------------------------------------------------------ */

#define GHOST_MIN_AGE_SEC 3

static bool wm_set_contains(const char *id) {
    struct BarState *bar = &g_bar;
    for (int i = 0; i < bar->wm_set_n; i++) {
        if (strcmp(bar->wm_set_ids[i], id) == 0) return true;
    }
    return false;
}

void bar_taskbar_prune_ghosts(void) {
    struct BarState *bar = &g_bar;
    if (!bar->wm_set_valid) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    bool removed = false;
    for (int i = 0; i < bar->toplevel_n; i++) {
        struct BarToplevel *tl = bar->toplevels[i];
        if (!tl || !tl->identifier[0]) continue;
        /* Guardas de corrida: só poda entradas com idade mínima e com base
         * num conjunto recebido DEPOIS da criação da entrada — janela recém-
         * aberta nunca é podada por um conjunto que ainda não a conhecia. */
        if (now.tv_sec - tl->created.tv_sec < GHOST_MIN_AGE_SEC) continue;
        if (bar->wm_set_time.tv_sec < tl->created.tv_sec ||
            (bar->wm_set_time.tv_sec == tl->created.tv_sec &&
             bar->wm_set_time.tv_nsec <= tl->created.tv_nsec)) continue;
        if (wm_set_contains(tl->identifier)) continue;

        LOG_WARN("taskbar: removendo fantasma \"%s\" app_id=%s id=%s (desconhecido pelo WM)",
            tl->title, tl->app_id, tl->identifier);
        taskbar_remove_entry(i);
        removed = true;
        i--;
    }
    if (removed) {
        bar_request_redraw_flags(&g_bar, BAR_DIRTY_TASKBAR);
        bar_update_render_suppressed();
    }
}

/* msg: "windows id1 id2 ..." (ou só "windows" = nenhuma janela viva).
 * Um id pode vir prefixado por um char de estado (autoritativo do WM):
 *   '!' = minimizada;  '+' = maximizada.
 * O prefixo é removido antes de guardar em wm_set_ids (a poda compara com o
 * identifier do foreign-toplevel, que não tem prefixo). O estado é aplicado ao
 * BarToplevel correspondente, tendo precedência sobre o zwlr. */
void bar_taskbar_set_wm_windows(const char *msg) {
    struct BarState *bar = &g_bar;
    const char *p = msg + 7; /* depois de "windows" */
    /* Conjuntos de ids deste lote, por estado, para aplicar aos toplevels. */
    char min_ids[BAR_MAX_TOPLEVELS][33];
    int  min_n = 0;
    char max_ids[BAR_MAX_TOPLEVELS][33];
    int  max_n = 0;
    char hidden_ids[BAR_MAX_TOPLEVELS][33];
    int  hidden_n = 0;
    int n = 0;
    while (*p != '\0' && n < BAR_MAX_TOPLEVELS) {
        while (*p == ' ' || *p == '\n') p++;
        if (*p == '\0') break;
        bool minimized = (*p == '!');
        bool maximized = (*p == '+');
        bool hidden = (*p == '#');
        if (minimized || maximized || hidden) p++; /* pula o prefixo */
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\n') p++;
        size_t len = (size_t)(p - start);
        if (len > 0 && len <= 32) {
            memcpy(bar->wm_set_ids[n], start, len);
            bar->wm_set_ids[n][len] = '\0';
            if (minimized && min_n < BAR_MAX_TOPLEVELS) {
                memcpy(min_ids[min_n], start, len);
                min_ids[min_n][len] = '\0';
                min_n++;
            }
            if (maximized && max_n < BAR_MAX_TOPLEVELS) {
                memcpy(max_ids[max_n], start, len);
                max_ids[max_n][len] = '\0';
                max_n++;
            }
            if (hidden && hidden_n < BAR_MAX_TOPLEVELS) {
                memcpy(hidden_ids[hidden_n], start, len);
                hidden_ids[hidden_n][len] = '\0';
                hidden_n++;
            }
            n++;
        }
    }
    bar->wm_set_n = n;
    bar->wm_set_valid = true;
    clock_gettime(CLOCK_MONOTONIC, &bar->wm_set_time);

    /* Aplica os estados minimizado/maximizado (precedência WM) a cada toplevel. */
    for (int i = 0; i < bar->toplevel_n; i++) {
        struct BarToplevel *tl = bar->toplevels[i];
        if (!tl || !tl->identifier[0]) continue;
        bool min = false;
        for (int j = 0; j < min_n; j++) {
            if (strcmp(min_ids[j], tl->identifier) == 0) { min = true; break; }
        }
        bool max = false;
        for (int j = 0; j < max_n; j++) {
            if (strcmp(max_ids[j], tl->identifier) == 0) { max = true; break; }
        }
        bool hid = false;
        for (int j = 0; j < hidden_n; j++) {
            if (strcmp(hidden_ids[j], tl->identifier) == 0) { hid = true; break; }
        }
        if (tl->minimized != min) {
            tl->minimized = min;
            bar_request_redraw_flags(bar, BAR_DIRTY_TASKBAR);
        }
        /* maximized/hidden não alteram o desenho do botão (sem redraw), só alimentam
         * o menu de contexto — mas atualizamos sempre para refletir o estado. */
        tl->maximized = max;
        tl->hidden = hid;
    }

    bar_taskbar_prune_ghosts();
}
