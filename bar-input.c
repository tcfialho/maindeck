#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "bar-state.h"
#include "bar-input.h"
#include "bar-taskbar.h"
#include "bar-quicklaunch.h"
#include "bar-render.h"
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

static void ptr_enter(void *data, struct wl_pointer *ptr,
    uint32_t serial, struct wl_surface *surf,
    wl_fixed_t fx, wl_fixed_t fy)
{
    (void)data; (void)ptr; (void)serial; (void)surf;
    struct BarState *bar = &g_bar;
    bar->ptr_x      = wl_fixed_to_double(fx);
    bar->ptr_y      = wl_fixed_to_double(fy);
    bar->ptr_inside = true;
}

static void ptr_leave(void *data, struct wl_pointer *ptr,
    uint32_t serial, struct wl_surface *surf)
{
    (void)data; (void)ptr; (void)serial; (void)surf;
    struct BarState *bar = &g_bar;
    bar->ptr_inside = false;
    if (bar->hover_hit != -1) {
        bar->hover_hit = -1;
        bar->dirty = true;
    }
}

static void ptr_motion(void *data, struct wl_pointer *ptr,
    uint32_t time, wl_fixed_t fx, wl_fixed_t fy)
{
    (void)data; (void)ptr; (void)time;
    struct BarState *bar = &g_bar;
    bar->ptr_x = wl_fixed_to_double(fx);
    bar->ptr_y = wl_fixed_to_double(fy);

    int hit = hit_test(bar->ptr_x, bar->ptr_y);
    if (hit != bar->hover_hit) {
        bar->hover_hit = hit;
        bar->dirty = true;
    }
}

static void ptr_button(void *data, struct wl_pointer *ptr,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)ptr; (void)serial; (void)time;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

    struct BarState *bar = &g_bar;
    int hit = hit_test(bar->ptr_x, bar->ptr_y);
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
        const char *mod = cfg->status[ha->index];
        if (left && strcmp(mod, "power") == 0 && cfg->power_exec[0]) {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                execlp("/bin/sh", "sh", "-c", cfg->power_exec, NULL);
                _exit(127);
            }
        }
        break;
    }
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
