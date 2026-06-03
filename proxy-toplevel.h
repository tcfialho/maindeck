#ifndef PROXY_TOPLEVEL_H
#define PROXY_TOPLEVEL_H

#include "proxy-types.h"

extern struct wl_list toplevels;
extern struct wl_list ext_toplevels;
extern pthread_mutex_t state_mu;
extern struct zwlr_foreign_toplevel_manager_v1 *proxy_manager;
extern struct ext_foreign_toplevel_list_v1 *ext_toplevel_list;

extern const struct wl_registry_listener reg_listener;

bool parse_bind(const uint8_t *msg, size_t msz,
    uint32_t *name, char *iface, size_t iface_cap,
    uint32_t *ver, uint32_t *new_id);

bool parse_global(const uint8_t *msg, size_t msz,
    uint32_t *name, char *iface, size_t iface_cap, uint32_t *ver);

size_t build_bind(uint8_t *out, size_t cap, uint32_t registry_id,
    uint32_t name, const char *iface, uint32_t version, uint32_t new_id);

#endif /* PROXY_TOPLEVEL_H */
