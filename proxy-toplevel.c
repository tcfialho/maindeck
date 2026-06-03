#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <wayland-client-core.h>

#include <wlr-foreign-toplevel-management-unstable-v1-client-protocol.h>
#include <ext-foreign-toplevel-list-v1-client-protocol.h>

#include "proxy-types.h"
#include "proxy-log.h"
#include "proxy-state.h"
#include "proxy-wire.h"
#include "proxy-emit.h"
#include "proxy-toplevel.h"

struct wl_list toplevels;
struct wl_list ext_toplevels;
pthread_mutex_t state_mu = PTHREAD_MUTEX_INITIALIZER;
struct zwlr_foreign_toplevel_manager_v1 *proxy_manager = NULL;
struct ext_foreign_toplevel_list_v1 *ext_toplevel_list = NULL;

static void broadcast_update(struct Toplevel *t) {
    if (!opt_synthesize) return;
    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (!c->active || !c->manager_n) continue;
        for (int m = 0; m < c->manager_n; m++) {
            uint32_t manager_id = c->manager_ids[m];
            uint32_t sid = client_sid(c, t->handle, manager_id);
            if (!sid) {
                sid = emit_toplevel_new(c, t->handle, manager_id);
                struct HEntry *entry = client_hentry_for_sid(c, sid);
                emit_str         (c, sid, 0, t->title   ? t->title   : "");
                emit_str         (c, sid, 1, t->app_id  ? t->app_id  : "");
                emit_state       (c, sid, t->activated);
                if (entry) emit_output_enter_once(c, entry);
                emit_done        (c, sid);
            } else {
                emit_str         (c, sid, 0, t->title   ? t->title   : "");
                emit_str         (c, sid, 1, t->app_id  ? t->app_id  : "");
                emit_state       (c, sid, t->activated);
                struct HEntry *entry = client_hentry_for_sid(c, sid);
                if (entry) emit_output_enter_once(c, entry);
                emit_done        (c, sid);
            }
        }
    }
    pthread_mutex_unlock(&clients_mu);
}

static void broadcast_closed(struct Toplevel *t) {
    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (!c->active || !c->manager_n) continue;
        for (int m = 0; m < c->manager_n; m++) {
            uint32_t manager_id = c->manager_ids[m];
            uint32_t sid = client_sid(c, t->handle, manager_id);
            if (sid) emit_closed(c, sid);
        }
    }
    pthread_mutex_unlock(&clients_mu);
}

static void tl_title(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h; struct Toplevel *t = data;
    pthread_mutex_lock(&state_mu);
    free(t->title); t->title = s ? strdup(s) : NULL;
    pthread_mutex_unlock(&state_mu);
}

static void tl_app_id(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h; struct Toplevel *t = data;
    pthread_mutex_lock(&state_mu);
    free(t->app_id); t->app_id = s ? strdup(s) : NULL;
    pthread_mutex_unlock(&state_mu);
}

static void tl_oe(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d;(void)h;(void)o; }

static void tl_ol(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d;(void)h;(void)o; }

static void tl_state(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st) {
    (void)h; struct Toplevel *t = data;
    bool act = false;
    uint32_t *s;
    wl_array_for_each(s, st)
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) act = true;
    pthread_mutex_lock(&state_mu);
    t->activated = act;
    pthread_mutex_unlock(&state_mu);
}

static void ext_unref(struct ExtToplevel *ext) {
    pthread_mutex_lock(&state_mu);
    int ref = --ext->refcount;
    pthread_mutex_unlock(&state_mu);
    if (ref == 0) {
        plog("ext_unref: identifier=%s — destruindo e liberando", ext->identifier);
        ext_foreign_toplevel_handle_v1_destroy(ext->handle);
        free(ext);
    }
}

static void tl_done(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h; struct Toplevel *t = data;
    pthread_mutex_lock(&state_mu);
    bool closed = t->closed;
    pthread_mutex_unlock(&state_mu);
    if (!closed) broadcast_update(t);
}

static void tl_closed(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h; struct Toplevel *t = data;
    plog("tl_closed: title='%s' ext=%p", t->title ? t->title : "", (void*)t->ext);
    broadcast_closed(t);
    pthread_mutex_lock(&state_mu);
    t->closed = true;
    wl_list_remove(&t->link);
    struct ExtToplevel *ext = t->ext;
    if (ext != NULL) {
        wl_list_remove(&ext->link);
        t->ext = NULL;
    }
    zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
    free(t->title); free(t->app_id);
    pthread_mutex_unlock(&state_mu);
    plog("tl_closed: done");
    free(t);
    if (ext != NULL) ext_unref(ext);
}

static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct zwlr_foreign_toplevel_handle_v1 *p) { (void)d;(void)h;(void)p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
    .title=tl_title, .app_id=tl_app_id,
    .output_enter=tl_oe, .output_leave=tl_ol,
    .state=tl_state, .done=tl_done,
    .closed=tl_closed, .parent=tl_parent,
};

static void mgr_toplevel(void *data,
    struct zwlr_foreign_toplevel_manager_v1 *mgr,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)data;(void)mgr;
    struct Toplevel *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->handle = h;
    t->ext = NULL;
    pthread_mutex_lock(&state_mu);
    int zwlr_count = 0;
    struct Toplevel *tt;
    wl_list_for_each(tt, &toplevels, link) { zwlr_count++; }
    int ext_idx = 0;
    struct ExtToplevel *ext;
    wl_list_for_each(ext, &ext_toplevels, link) {
        if (ext_idx == zwlr_count) {
            t->ext = ext;
            break;
        }
        ext_idx++;
    }
    wl_list_insert(toplevels.prev, &t->link);
    pthread_mutex_unlock(&state_mu);
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, t);
    if (t->ext != NULL) {
        plog("mgr_toplevel: zwlr handle linkado ao ext identifier=%s",
             t->ext->identifier[0] ? t->ext->identifier : "(pendente)");
    } else {
        plog("mgr_toplevel: aviso — ext não encontrado para novo zwlr (ext_count=%d zwlr_count=%d)",
             ext_idx, zwlr_count);
    }
}

static void mgr_finished(void *d,
    struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d;(void)m; }

static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_listener = {
    .toplevel=mgr_toplevel, .finished=mgr_finished,
};

static void ext_handle_identifier(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *identifier) {
    (void)handle;
    struct ExtToplevel *ext = data;
    if (identifier == NULL || identifier[0] == '\0') return;
    pthread_mutex_lock(&state_mu);
    snprintf(ext->identifier, sizeof(ext->identifier), "%s", identifier);
    int ext_idx = 0;
    struct ExtToplevel *e;
    wl_list_for_each(e, &ext_toplevels, link) {
        if (e == ext) break;
        ext_idx++;
    }
    int zwlr_idx = 0;
    struct Toplevel *ttt;
    wl_list_for_each(ttt, &toplevels, link) {
        if (zwlr_idx == ext_idx) {
            if (ttt->ext == NULL) ttt->ext = ext;
            plog("ext_handle_identifier: identifier=%s linkado ao zwlr idx=%d",
                 identifier, ext_idx);
            break;
        }
        zwlr_idx++;
    }
    pthread_mutex_unlock(&state_mu);
}

static void ext_handle_title(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *title) {
    (void)data; (void)handle; (void)title;
}

static void ext_handle_app_id(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *app_id) {
    (void)data; (void)handle; (void)app_id;
}

static void ext_handle_done(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)data; (void)handle;
}

static void ext_handle_closed(void *data,
    struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    struct ExtToplevel *ext = data;
    plog("ext_handle_closed: identifier=%s", ext->identifier);
    pthread_mutex_lock(&state_mu);
    if (ext->link.next && ext->link.next != &ext->link) {
        wl_list_remove(&ext->link);
        wl_list_init(&ext->link);
    }
    pthread_mutex_unlock(&state_mu);
    ext_unref(ext);
}

static const struct ext_foreign_toplevel_handle_v1_listener ext_handle_listener = {
    .identifier = ext_handle_identifier,
    .title      = ext_handle_title,
    .app_id     = ext_handle_app_id,
    .done       = ext_handle_done,
    .closed     = ext_handle_closed,
};

static void ext_list_toplevel(void *data,
    struct ext_foreign_toplevel_list_v1 *list,
    struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)data; (void)list;
    struct ExtToplevel *ext = calloc(1, sizeof(*ext));
    if (!ext) return;
    ext->handle = handle;
    ext->identifier[0] = '\0';
    ext->refcount = 2;
    wl_list_init(&ext->link);
    pthread_mutex_lock(&state_mu);
    wl_list_insert(ext_toplevels.prev, &ext->link);
    pthread_mutex_unlock(&state_mu);
    ext_foreign_toplevel_handle_v1_add_listener(handle, &ext_handle_listener, ext);
}

static void ext_list_finished(void *data,
    struct ext_foreign_toplevel_list_v1 *list) {
    (void)data; (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener ext_list_listener = {
    .toplevel = ext_list_toplevel,
    .finished = ext_list_finished,
};

static void reg_global(void *d, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
        proxy_manager = wl_registry_bind(reg, name,
            &zwlr_foreign_toplevel_manager_v1_interface, ver<3?ver:3);
        zwlr_foreign_toplevel_manager_v1_add_listener(
            proxy_manager, &mgr_listener, NULL);
    } else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name)) {
        ext_toplevel_list = wl_registry_bind(reg, name,
            &ext_foreign_toplevel_list_v1_interface, ver < 1 ? ver : 1);
        ext_foreign_toplevel_list_v1_add_listener(
            ext_toplevel_list, &ext_list_listener, NULL);
    }
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }

const struct wl_registry_listener reg_listener = {
    .global=reg_global, .global_remove=reg_remove
};

bool parse_bind(const uint8_t *msg, size_t msz,
    uint32_t *name, char *iface, size_t iface_cap,
    uint32_t *ver, uint32_t *new_id) {
    if (msz < MSG_HDR + 4u) return false;
    size_t off = MSG_HDR;
    *name = ru32(msg+off); off += 4;
    if (off+4 > msz) return false;
    uint32_t slen = ru32(msg+off); off += 4;
    uint32_t pad  = (slen+3u)&~3u;
    if (off+pad+8 > msz) return false;
    size_t cp = slen < iface_cap ? slen : iface_cap-1;
    memcpy(iface, msg+off, cp); iface[cp] = '\0';
    off += pad;
    *ver    = ru32(msg+off); off += 4;
    *new_id = ru32(msg+off);
    return true;
}

bool parse_global(const uint8_t *msg, size_t msz,
    uint32_t *name, char *iface, size_t iface_cap, uint32_t *ver) {
    if (msz < MSG_HDR + 4u) return false;
    size_t off = MSG_HDR;
    *name = ru32(msg+off); off += 4;
    if (off+4 > msz) return false;
    uint32_t slen = ru32(msg+off); off += 4;
    uint32_t pad  = (slen+3u)&~3u;
    if (off+pad+4 > msz) return false;
    size_t cp = slen < iface_cap ? slen : iface_cap-1;
    memcpy(iface, msg+off, cp); iface[cp] = '\0';
    off += pad;
    *ver = ru32(msg+off);
    return true;
}

size_t build_bind(uint8_t *out, size_t cap, uint32_t registry_id,
    uint32_t name, const char *iface, uint32_t version, uint32_t new_id) {
    if (cap < MSG_HDR + 16u) return 0;
    uint32_t off = 8;
    wu32(out + off, name); off += 4;
    int sl = encode_str(out + off, cap - off - 8, iface);
    if (sl < 0) return 0;
    off += (uint32_t)sl;
    wu32(out + off, version); off += 4;
    wu32(out + off, new_id); off += 4;
    wu32(out+0, registry_id);
    wu32(out+4, (off<<16)|0u);
    return off;
}
