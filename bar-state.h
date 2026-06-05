#ifndef BAR_STATE_H
#define BAR_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"

#include "bar-config.h"

/* ------------------------------------------------------------------ */
/* Toplevel                                                             */
/* ------------------------------------------------------------------ */

#define BAR_MAX_TOPLEVELS 64

struct BarToplevel {
    struct zwlr_foreign_toplevel_handle_v1 *zwlr_handle;
    struct ext_foreign_toplevel_handle_v1  *ext_handle;
    char   identifier[33];
    char   title[256];
    char   app_id[128];
    bool   activated;
    bool   minimized;
    bool   closed;
    cairo_surface_t *icon_surface; /* cached, owned by bar-icons */
};

/* ------------------------------------------------------------------ */
/* Hit areas (for input)                                               */
/* ------------------------------------------------------------------ */

#define BAR_MAX_HITS 128

typedef enum {
    HIT_NONE = 0,
    HIT_QL,        /* quick-launch button */
    HIT_TASKBAR,   /* taskbar window */
    HIT_STATUS,    /* status module (vol, bat, clock, power) */
    HIT_TRAY,      /* system tray item */
} HitType;

struct HitArea {
    HitType type;
    int     index;   /* QL index, taskbar index, or status index */
    int     x, y, w, h;
};

/* ------------------------------------------------------------------ */
/* Global bar state                                                     */
/* ------------------------------------------------------------------ */

struct BarState {
    /* Wayland globals */
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct wl_seat       *seat;
    struct wl_pointer    *pointer;
    struct wl_output     *output;

    /* Layer shell */
    struct zwlr_layer_shell_v1         *layer_shell;
    struct zwlr_layer_surface_v1       *layer_surface;
    struct wl_surface                  *wl_surface;

    /* Toplevel management */
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr;
    struct ext_foreign_toplevel_list_v1     *ext_list;

    /* Buffers */
    struct wl_shm_pool  *shm_pool;
    struct wl_buffer    *buf[2];
    void                *shm_data;
    int                  shm_fd;
    int                  buf_width, buf_height, buf_stride, buf_size;
    int                  cur_buf;

    /* Toplevels */
    struct BarToplevel toplevels[BAR_MAX_TOPLEVELS];
    int                toplevel_n;

    /* Ext toplevels: parallel array by arrival order */
    struct ext_foreign_toplevel_handle_v1 *ext_handles[BAR_MAX_TOPLEVELS];
    char ext_identifiers[BAR_MAX_TOPLEVELS][33];
    int  ext_n;

    /* Bar geometry */
    int width, height;
    bool configured;
    bool dirty;        /* needs redraw */

    /* Status */
    char vol_text[32];
    char bat_text[32];
    char clock_text[32];
    int  bat_level;    /* 0-100, -1 = unknown */
    bool bat_charging; /* true if Charging or Full */

    /* Pointer state */
    double  ptr_x, ptr_y;
    bool    ptr_inside;
    int     hover_hit;    /* index in hit_areas, -1 if none */
    HitType hover_type;   /* type of currently hovered element */
    int     hover_index;  /* index within that type, -1 if none */
    uint32_t last_btn_serial; /* serial from last ptr_button press */
    uint32_t last_btn_time;   /* time from last ptr_button press */

    /* Hit areas */
    struct HitArea hit_areas[BAR_MAX_HITS];
    int            hit_n;

    /* xdg_shell (for popup menus) */
    struct xdg_wm_base *xdg_wm_base;

    /* Popup menu state (tray context menu) */
    struct wl_surface         *menu_surface;
    struct xdg_surface        *menu_xdg_surface;
    struct xdg_popup          *menu_popup;
    struct wl_shm_pool        *menu_pool;
    struct wl_buffer          *menu_buf;
    void                      *menu_data;
    int                        menu_fd;
    int                        menu_width, menu_height, menu_stride;
    bool                       menu_open;
    int                        menu_tray_idx;  /* which tray item opened the menu */
    bool                       ptr_on_menu;    /* pointer is over the menu surface */
    double                     menu_ptr_x, menu_ptr_y;
    int                        menu_hover_row; /* -1 = none */

    /* IPC socket */
    int ipc_sock;  /* AF_UNIX SOCK_DGRAM, -1 if not connected */
    char ipc_path[108];

    /* Config */
    struct BarConfig config;

    /* Background surface (for Fuzzel empty space click-to-dismiss) */
    struct wp_viewporter           *viewporter;
    struct wl_surface              *bg_surface;
    struct zwlr_layer_surface_v1   *bg_layer_surface;
    struct wp_viewport             *bg_viewport;
    struct wl_buffer               *bg_buffer;
    bool                            bg_buffer_is_fullsize;
    int                             bg_width, bg_height;
};

/* Global instance */
extern struct BarState g_bar;

#endif /* BAR_STATE_H */
