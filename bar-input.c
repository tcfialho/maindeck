#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "bar-state.h"
#include "bar-input.h"
#include "bar-taskbar.h"
#include "bar-quicklaunch.h"
#include "bar-tray.h"
#include "bar-log.h"

static int hit_test(double x, double y) {
    struct BarState *bar = &g_bar;
    for (int i = 0; i < bar->hit_n; i++) {
        struct HitArea *h = &bar->hit_areas[i];
        if (x >= h->x && x < h->x + h->w &&
            y >= h->y && y < h->y + h->h)
            return i;
    }
    return -1;
}

static void set_default_cursor(uint32_t serial) {
    struct BarState *bar = &g_bar;
    if (bar->cursor_shape_device) {
        wp_cursor_shape_device_v1_set_shape(
            bar->cursor_shape_device,
            serial,
            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    }
}

static void clear_bar_hover(struct BarState *bar) {
    if (bar->hover_hit == -1) return;
    HitType old_type = bar->hover_type;
    bar->hover_hit   = -1;
    bar->hover_type  = HIT_NONE;
    bar->hover_index = -1;

    uint32_t flags = BAR_DIRTY_NONE;
    if (old_type == HIT_QL) flags |= BAR_DIRTY_QUICKLAUNCH;
    else if (old_type == HIT_TASKBAR) flags |= BAR_DIRTY_TASKBAR;
    else if (old_type == HIT_TRAY) flags |= BAR_DIRTY_TRAY;
    else if (old_type == HIT_STATUS) flags |= BAR_DIRTY_STATUS;

    bar_request_redraw_flags(bar, flags);
}

static void ptr_enter(void *data, struct wl_pointer *ptr,
    uint32_t serial, struct wl_surface *surf,
    wl_fixed_t fx, wl_fixed_t fy)
{
    (void)data; (void)ptr;
    struct BarState *bar = &g_bar;

    bar->ptr_surface = surf;
    if (bar->menu_open && surf == bar->menu_surface) {
        set_default_cursor(serial);
        bar->ptr_on_menu  = true;
        bar->menu_ptr_x   = wl_fixed_to_double(fx);
        bar->menu_ptr_y   = wl_fixed_to_double(fy);
        return;
    }

    bar->ptr_on_menu = false;
    if (surf == bar->wl_surface) {
        set_default_cursor(serial);
        bar->ptr_x      = wl_fixed_to_double(fx);
        bar->ptr_y      = wl_fixed_to_double(fy);
        bar->ptr_inside = true;
        return;
    }

    if (surf == bar->bg_surface) {
        set_default_cursor(serial);
    }
    bar->ptr_inside = false;
    clear_bar_hover(bar);
}

static void ptr_leave(void *data, struct wl_pointer *ptr,
    uint32_t serial, struct wl_surface *surf)
{
    (void)data; (void)ptr; (void)serial;
    struct BarState *bar = &g_bar;
    if (bar->ptr_surface == surf) {
        bar->ptr_surface = NULL;
    }
    if (bar->menu_open && surf == bar->menu_surface) {
        bar->ptr_on_menu    = false;
        bar->menu_hover_row = -1;
        bar_tray_menu_rerend();
        return;
    }

    if (surf == bar->wl_surface) {
        bar->ptr_inside = false;
        clear_bar_hover(bar);
    }
}

static void ptr_motion(void *data, struct wl_pointer *ptr,
    uint32_t time, wl_fixed_t fx, wl_fixed_t fy)
{
    (void)data; (void)ptr; (void)time;
    struct BarState *bar = &g_bar;

    if (bar->ptr_surface == bar->menu_surface && bar->ptr_on_menu && bar->menu_open) {
        bar->menu_ptr_x = wl_fixed_to_double(fx);
        bar->menu_ptr_y = wl_fixed_to_double(fy);
        int row = bar_tray_menu_row_at((int)bar->menu_ptr_y);
        if (row != bar->menu_hover_row) {
            bar->menu_hover_row = row;
            bar_tray_menu_rerend();
        }
        return;
    }

    if (bar->ptr_surface != bar->wl_surface) {
        return;
    }

    bar->ptr_x = wl_fixed_to_double(fx);
    bar->ptr_y = wl_fixed_to_double(fy);

    int hit = hit_test(bar->ptr_x, bar->ptr_y);
    if (hit != bar->hover_hit) {
        HitType old_type = bar->hover_type;
        bar->hover_hit = hit;
        if (hit >= 0 && hit < bar->hit_n) {
            bar->hover_type  = bar->hit_areas[hit].type;
            bar->hover_index = bar->hit_areas[hit].index;
        } else {
            bar->hover_type  = HIT_NONE;
            bar->hover_index = -1;
        }

        uint32_t flags = BAR_DIRTY_NONE;
        if (old_type == HIT_QL || bar->hover_type == HIT_QL) flags |= BAR_DIRTY_QUICKLAUNCH;
        if (old_type == HIT_TASKBAR || bar->hover_type == HIT_TASKBAR) flags |= BAR_DIRTY_TASKBAR;
        if (old_type == HIT_TRAY || bar->hover_type == HIT_TRAY) flags |= BAR_DIRTY_TRAY;
        if (old_type == HIT_STATUS || bar->hover_type == HIT_STATUS) flags |= BAR_DIRTY_STATUS;

        bar_request_redraw_flags(bar, flags);
    }
}

static void ptr_button(void *data, struct wl_pointer *ptr,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)ptr;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

    struct BarState *bar = &g_bar;
    bar->last_btn_serial = serial;
    bar->last_btn_time   = time;

    /* Click on menu surface */
    if (bar->ptr_surface == bar->menu_surface && bar->ptr_on_menu && bar->menu_open) {
        if (button == 0x110) {
            int row = bar_tray_menu_row_at((int)bar->menu_ptr_y);
            bar_tray_menu_activate(row, time);
        }
        return;
    }

    if (bar->ptr_surface != bar->wl_surface) {
        return;
    }

    int hit = hit_test(bar->ptr_x, bar->ptr_y);
    LOG_INFO("input: button=0x%x ptr=(%.0f,%.0f) hit=%d hit_n=%d",
             button, bar->ptr_x, bar->ptr_y, hit, bar->hit_n);
    if (hit < 0 || hit >= bar->hit_n) return;

    struct HitArea *ha = &bar->hit_areas[hit];

    /* BTN_LEFT = 0x110, BTN_MIDDLE = 0x112 */
    bool left   = (button == 0x110);
    bool middle = (button == 0x112);

    switch (ha->type) {
    case HIT_QL:
        if (left) bar_quicklaunch_exec(ha->index);
        break;
    case HIT_TASKBAR:
        if (left)   bar_taskbar_activate(ha->index);
        if (middle) bar_taskbar_close(ha->index);
        break;
    case HIT_STATUS: {
        struct BarConfig *cfg = &bar->config;
        if (ha->index < 0 || ha->index >= cfg->status_n) break;
        BarStatusModule mod = cfg->status[ha->index];
        if (left && mod == BAR_STATUS_POWER && cfg->power_exec[0]) {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                const char *home = getenv("HOME");
                if (home) chdir(home);
                execlp("/bin/sh", "sh", "-c", cfg->power_exec, NULL);
                _exit(127);
            }
        } else if (left && mod == BAR_STATUS_VOLUME) {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                const char *home = getenv("HOME");
                if (home) chdir(home);
                execlp("/bin/sh", "sh", "-c", cfg->volume_exec, NULL);
                _exit(127);
            }
        }
        break;
    }
    case HIT_TRAY:
        /* Any click opens the dbusmenu if available, else Activate */
        bar_tray_open_menu(ha->index, ha->x, ha->w, serial);
        break;
    case HIT_NONE:
        break;
    }
}

static void ptr_axis(void *d, struct wl_pointer *p,
    uint32_t t, uint32_t a, wl_fixed_t v) {
    (void)d; (void)p; (void)t; (void)a; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ptr_axis_relative_direction(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) { (void)d; (void)p; (void)a; (void)dir; }

const struct wl_pointer_listener bar_pointer_listener = {
    .enter                  = ptr_enter,
    .leave                  = ptr_leave,
    .motion                 = ptr_motion,
    .button                 = ptr_button,
    .axis                   = ptr_axis,
    .frame                  = ptr_frame,
    .axis_source            = ptr_axis_source,
    .axis_stop              = ptr_axis_stop,
    .axis_discrete          = ptr_axis_discrete,
    .axis_value120          = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_relative_direction,
};
