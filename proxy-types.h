#ifndef PROXY_TYPES_H
#define PROXY_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#define MAX_CLIENTS 8
#define HANDLE_MAP  256
#define SERVER_ID_BASE 0xFFF00000u
#define FAKE_OUTPUT_GLOBAL 0xE0000000u

struct ExtToplevel {
    struct ext_foreign_toplevel_handle_v1 *handle;
    char   identifier[33];
    int    refcount;  /* inicia em 2: 1 pelo Toplevel zwlr + 1 pelo ext handle */
    struct wl_list link;
};

struct Toplevel {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char   *title;
    char   *app_id;
    bool    activated;
    bool    closed;
    struct ExtToplevel *ext; /* ponteiro direto para o ext correspondente (lockstep) */
    struct wl_list link;
};

struct HEntry {
    uint32_t cid;
    uint32_t manager_id;
    struct zwlr_foreign_toplevel_handle_v1 *h;
    bool output_entered;
};

struct Client {
    int            waybar_fd;
    int            river_fd;
    pthread_mutex_t write_mu;

    uint32_t       manager_ids[8]; /* waybar's object IDs for our managers */
    int            manager_n;
    uint32_t       output_id;    /* waybar's first wl_output object ID (0=not seen) */
    uint32_t       output_version;
    bool           output_is_fake;
    uint32_t       compositor_global;
    uint32_t       compositor_version;
    uint32_t       compositor_id;
    uint32_t       fixes_global;
    uint32_t       fixes_version;
    uint32_t       zxdg_output_manager_id;
    uint32_t       zxdg_output_manager_version;
    uint32_t       zwlr_layer_shell_id;
    uint32_t       data_device_manager_id;
    bool           fake_output_global_sent;
    uint32_t       fake_output_registry_id;

    struct HEntry  hmap[HANDLE_MAP];
    int            hmap_n;
    uint32_t       drop_ids[HANDLE_MAP];
    int            drop_n;
    /* IDs assigned by waybar for layer surfaces (via get_layer_surface new_id).
     * These client-side IDs are echoed back by River in configure events and must
     * NEVER be dropped even if they collide with a toplevel handle drop_id. */
    uint32_t       layer_surface_ids[64];
    int            layer_surface_n;
    uint32_t       registry_ids[32];
    int            registry_n;
    char           unhandled_binds[32][128]; /* iface names already logged (matches iface[128] parse buffer) */
    int            unhandled_n;
    uint32_t       next_sid;     /* next server-side ID for handle events */

    /* Direction B: River's REAL foreign-toplevel handle IDs as seen on THIS
     * waybar connection (parsed from the s2c stream: manager.toplevel new_id).
     * We forward River's handles untouched and inject output_enter on these
     * real IDs — no synthetic server-id allocation, so no libwayland EINVAL. */
    uint32_t       real_handle_ids[HANDLE_MAP];
    bool           real_handle_entered[HANDLE_MAP]; /* output_enter already injected */
    int            real_handle_n;

    bool           output_done_received;
    int            active_threads;
    bool           active;
};

struct RelayArgs {
    struct Client *c;
    int src_fd;
    int dst_fd;
};

#endif /* PROXY_TYPES_H */
