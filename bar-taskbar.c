#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#include "bar-state.h"
#include "bar-taskbar.h"
#include "bar-icons.h"
#include "bar-render.h"
#include "bar-log.h"

/* ------------------------------------------------------------------ */
/* zwlr handlers                                                        */
/* ------------------------------------------------------------------ */

static void tl_title(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h;
    struct BarToplevel *tl = data;
    snprintf(tl->title, sizeof(tl->title), "%s", s ? s : "");
    g_bar.dirty = true;
}

static void tl_app_id(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h;
    struct BarToplevel *tl = data;
    snprintf(tl->app_id, sizeof(tl->app_id), "%s", s ? s : "");
    /* Load icon on first app_id */
    if (!tl->icon_surface && s && s[0]) {
        tl->icon_surface = bar_icon_get(s, 18);
    }
    g_bar.dirty = true;
}

static void tl_oe(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d; (void)h; (void)o; }

static void tl_ol(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d; (void)h; (void)o; }

static void tl_state(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st) {
    (void)h;
    struct BarToplevel *tl = data;
    bool act = false, min = false;
    uint32_t *s;
    wl_array_for_each(s, st) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) act = true;
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)  min = true;
    }
    tl->activated = act;
    tl->minimized  = min;
    g_bar.dirty = true;
}

static void tl_done(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h; (void)data;
    g_bar.dirty = true;
}

static void tl_closed(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    struct BarState *bar = &g_bar;

    /* Find by handle pointer — data may be stale after prior shifts */
    int idx = -1;
    for (int i = 0; i < bar->toplevel_n; i++) {
        if (bar->toplevels[i].zwlr_handle == h) { idx = i; break; }
    }

    if (idx >= 0) {
        int rem = bar->toplevel_n - idx - 1;
        if (rem > 0)
            memmove(&bar->toplevels[idx], &bar->toplevels[idx+1],
                    (size_t)rem * sizeof(bar->toplevels[0]));
        bar->toplevel_n--;
    }

    zwlr_foreign_toplevel_handle_v1_destroy(h);
    g_bar.dirty = true;
    (void)data;
    LOG_INFO("taskbar: toplevel closed, remaining=%d", g_bar.toplevel_n);
}

static void tl_parent(void *d,
    struct zwlr_foreign_toplevel_handle_v1 *h,
    struct zwlr_foreign_toplevel_handle_v1 *p) {
    (void)d; (void)h; (void)p;
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
    if (bar->toplevel_n >= BAR_MAX_TOPLEVELS) {
        LOG_WARN("taskbar: too many toplevels, ignoring");
        return;
    }

    int idx = bar->toplevel_n;
    struct BarToplevel *tl = &bar->toplevels[idx];
    memset(tl, 0, sizeof(*tl));
    tl->zwlr_handle = h;

    /* Lockstep: if ext already arrived at same position, link it */
    if (idx < bar->ext_n) {
        tl->ext_handle = bar->ext_handles[idx];
        snprintf(tl->identifier, sizeof(tl->identifier), "%s", bar->ext_identifiers[idx]);
    }

    bar->toplevel_n++;
    bar->dirty = true;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, tl);

    LOG_INFO("taskbar: new toplevel idx=%d (total=%d)", idx, bar->toplevel_n);
}

static void mgr_finished(void *d,
    struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }

const struct zwlr_foreign_toplevel_manager_v1_listener bar_mgr_listener = {
    .toplevel = mgr_toplevel,
    .finished = mgr_finished,
};

/* ------------------------------------------------------------------ */
/* ext-foreign-toplevel handlers                                        */
/* ------------------------------------------------------------------ */

static void ext_identifier(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *id) {
    (void)handle;
    struct BarState *bar = &g_bar;

    /* Find which ext slot this is */
    int ext_idx = -1;
    for (int i = 0; i < bar->ext_n; i++) {
        if (bar->ext_handles[i] == handle) { ext_idx = i; break; }
    }
    if (ext_idx < 0) return;

    snprintf(bar->ext_identifiers[ext_idx], 33, "%s", id ? id : "");

    /* Link to zwlr at same position if it exists */
    if (ext_idx < bar->toplevel_n) {
        struct BarToplevel *tl = &bar->toplevels[ext_idx];
        tl->ext_handle = handle;
        snprintf(tl->identifier, sizeof(tl->identifier), "%s", id ? id : "");
        LOG_INFO("taskbar: ext identifier=%s linked to zwlr idx=%d", id ? id : "", ext_idx);
    }
}

static void ext_title(void *d,
    struct ext_foreign_toplevel_handle_v1 *h, const char *t) {
    (void)d; (void)h; (void)t;
}

static void ext_app_id(void *d,
    struct ext_foreign_toplevel_handle_v1 *h, const char *a) {
    (void)d; (void)h; (void)a;
}

static void ext_done(void *d,
    struct ext_foreign_toplevel_handle_v1 *h) {
    (void)d; (void)h;
}

static void ext_closed(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle) {
    struct BarState *bar = &g_bar;
    int ext_idx = -1;
    for (int i = 0; i < bar->ext_n; i++) {
        if (bar->ext_handles[i] == handle) { ext_idx = i; break; }
    }
    if (ext_idx < 0) return;

    /* Shift ext arrays */
    int rem = bar->ext_n - ext_idx - 1;
    if (rem > 0) {
        memmove(&bar->ext_handles[ext_idx],
                &bar->ext_handles[ext_idx+1],
                (size_t)rem * sizeof(bar->ext_handles[0]));
        memmove(&bar->ext_identifiers[ext_idx],
                &bar->ext_identifiers[ext_idx+1],
                (size_t)rem * sizeof(bar->ext_identifiers[0]));
    }
    bar->ext_n--;

    ext_foreign_toplevel_handle_v1_destroy(handle);
    (void)data;
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
    if (bar->ext_n >= BAR_MAX_TOPLEVELS) return;

    int idx = bar->ext_n;
    bar->ext_handles[idx]        = handle;
    bar->ext_identifiers[idx][0] = '\0';
    bar->ext_n++;

    ext_foreign_toplevel_handle_v1_add_listener(handle, &ext_handle_listener, handle);
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

void bar_taskbar_activate(int idx) {
    struct BarState *bar = &g_bar;
    if (idx < 0 || idx >= bar->toplevel_n) return;

    struct BarToplevel *tl = &bar->toplevels[idx];
    if (!tl->identifier[0]) {
        LOG_WARN("taskbar: no identifier for idx=%d, cannot activate", idx);
        return;
    }

    if (bar->ipc_sock < 0) {
        LOG_WARN("taskbar: IPC socket not connected");
        return;
    }

    char msg[64];
    int  len = snprintf(msg, sizeof(msg), "activate %s", tl->identifier);

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

void bar_taskbar_close(int idx) {
    struct BarState *bar = &g_bar;
    if (idx < 0 || idx >= bar->toplevel_n) return;
    struct BarToplevel *tl = &bar->toplevels[idx];
    if (tl->zwlr_handle) {
        zwlr_foreign_toplevel_handle_v1_close(tl->zwlr_handle);
    }
}
