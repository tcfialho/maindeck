#ifndef BAR_STATE_H
#define BAR_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <wayland-client.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"

#include <pango/pango.h>
#include "bar-config.h"

/* ------------------------------------------------------------------ */
/* Toplevel                                                             */
/* ------------------------------------------------------------------ */

#define BAR_MAX_TOPLEVELS 64

struct BarToplevel {
    struct zwlr_foreign_toplevel_handle_v1 *zwlr_handle;
    struct ext_foreign_toplevel_handle_v1  *ext_handle;
    struct timespec created; /* CLOCK_MONOTONIC — guarda de corrida do ceifador */
    char   identifier[33];
    char   title[256];
    char   app_id[128];
    bool   activated;
    bool   minimized;
    bool   maximized;  /* autoritativo do WM (prefixo '+' em 'windows') */
    bool   fullscreen;
    bool   has_parent;
    bool   closed;
    cairo_surface_t *icon_surface; /* cached, owned by bar-icons */
    PangoLayout     *layout;
    char             last_title_shaped[256];
    int              last_width_shaped;
    int              last_tw_shaped;
    int              last_th_shaped;
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

enum BarDirtyFlags {
    BAR_DIRTY_NONE        = 0,
    BAR_DIRTY_QUICKLAUNCH = 1 << 0,
    BAR_DIRTY_TASKBAR     = 1 << 1,
    BAR_DIRTY_TRAY        = 1 << 2,
    BAR_DIRTY_STATUS      = 1 << 3,
    BAR_DIRTY_ALL         = 1 << 4
};

#define BAR_SECTION_QUICKLAUNCH 0
#define BAR_SECTION_TASKBAR     1
#define BAR_SECTION_TRAY        2
#define BAR_SECTION_STATUS      3

struct BarSectionBox {
    int32_t x1, x2;
};

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
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1  *cursor_shape_device;

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
    struct BarToplevel *toplevels[BAR_MAX_TOPLEVELS];
    int                toplevel_n;

    /* Ext toplevels: parallel array by arrival order */
    struct ext_foreign_toplevel_handle_v1 *ext_handles[BAR_MAX_TOPLEVELS];
    char ext_identifiers[BAR_MAX_TOPLEVELS][33];
    int  ext_n;

    /* Conjunto de janelas vivas segundo o WM (ceifador de fantasmas).
     * Atualizado via "windows id1 id2 ..." no notify socket. */
    char wm_set_ids[BAR_MAX_TOPLEVELS][33];
    int  wm_set_n;
    bool wm_set_valid;
    struct timespec wm_set_time;

    /* Bar geometry */
    int width, height;
    bool configured;
    bool dirty;        /* needs redraw */
    bool dirty_deferred;
    bool render_suppressed;
    uint32_t dirty_flags;
    struct BarSectionBox section_box[4];
    struct BarSectionBox section_box_prev[4];
    bool prev_boxes_valid;

    /* Status */
    char vol_text[32];
    char bat_text[32];
    char clock_text[32];
    int  bat_level;    /* 0-100, -1 = unknown */
    bool bat_charging; /* true if Charging or Full */

    /* Pointer state */
    double  ptr_x, ptr_y;
    bool    ptr_inside;
    struct wl_surface *ptr_surface;
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

    /* IPC socket (bar→wm: activate) */
    int ipc_sock;  /* AF_UNIX SOCK_DGRAM, -1 if not connected */
    char ipc_path[108];

    /* Notify socket (wm→bar: fullscreen_on/off) */
    int notify_sock; /* AF_UNIX SOCK_DGRAM server, -1 if not open */
    bool wm_fullscreen; /* WM-side fullscreen override; takes precedence over zwlr state */

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

static inline void bar_request_redraw_flags(struct BarState *bar, uint32_t flags) {
    bar->dirty_flags |= flags;
    if (bar->render_suppressed) {
        bar->dirty = false;
        bar->dirty_deferred = true;
    } else {
        bar->dirty = true;
    }
}

static inline void bar_request_redraw(struct BarState *bar) {
    bar_request_redraw_flags(bar, BAR_DIRTY_ALL);
}

#endif /* BAR_STATE_H */
