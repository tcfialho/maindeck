#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "bar-state.h"
#include "bar-surface.h"
#include "bar-log.h"

/* ------------------------------------------------------------------ */
/* Shared memory helpers                                                */
/* ------------------------------------------------------------------ */

static int create_shm_file(size_t size) {
    char name[64];
    snprintf(name, sizeof(name), "/maindeck-bar-%d", (int)getpid());
    int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        /* fallback: anonymous fd via memfd_create */
        fd = memfd_create("maindeck-bar", MFD_CLOEXEC);
        if (fd < 0) { LOG_ERR("surface: memfd_create: %m"); return -1; }
    } else {
        shm_unlink(name);
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        LOG_ERR("surface: ftruncate: %m");
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* Layer surface callbacks                                              */
/* ------------------------------------------------------------------ */

static void layer_surface_configure(void *data,
    struct zwlr_layer_surface_v1 *surf,
    uint32_t serial, uint32_t w, uint32_t h)
{
    (void)data;
    struct BarState *bar = &g_bar;

    zwlr_layer_surface_v1_ack_configure(surf, serial);

    int new_w = (int)(w > 0 ? w : (uint32_t)bar->width);
    int new_h = (int)(h > 0 ? h : (uint32_t)bar->height);
    if (new_w == 0) new_w = 1920; /* safe fallback until output geometry arrives */

    if (!bar->configured || new_w != bar->buf_width || new_h != bar->buf_height) {
        bar_surface_resize(new_w, new_h);
    }
    bar->configured = true;
    bar->dirty = true;
}

static void layer_surface_closed(void *data,
    struct zwlr_layer_surface_v1 *surf)
{
    (void)data; (void)surf;
    LOG_INFO("surface: layer surface closed — exiting");
    exit(0);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* ------------------------------------------------------------------ */
/* Public: create surface                                               */
/* ------------------------------------------------------------------ */

void bar_surface_create(void) {
    struct BarState *bar = &g_bar;

    bar->wl_surface = wl_compositor_create_surface(bar->compositor);
    if (!bar->wl_surface) { LOG_ERR("surface: wl_compositor_create_surface failed"); exit(1); }

    bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        bar->layer_shell,
        bar->wl_surface,
        NULL,   /* output = NULL → compositor picks */
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "maindeck-bar"
    );
    if (!bar->layer_surface) { LOG_ERR("surface: get_layer_surface failed"); exit(1); }

    zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->height);
    zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, (uint32_t)bar->height);
    zwlr_layer_surface_v1_set_keyboard_interactivity(bar->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);

    zwlr_layer_surface_v1_add_listener(bar->layer_surface,
        &layer_surface_listener, NULL);

    wl_surface_commit(bar->wl_surface);
    LOG_INFO("surface: layer surface created (h=%d, exclusive_zone=%d)", bar->height, bar->height);
}

/* ------------------------------------------------------------------ */
/* Public: resize / allocate buffers                                    */
/* ------------------------------------------------------------------ */

void bar_surface_resize(int w, int h) {
    struct BarState *bar = &g_bar;

    /* Free old buffers */
    if (bar->buf[0]) { wl_buffer_destroy(bar->buf[0]); bar->buf[0] = NULL; }
    if (bar->buf[1]) { wl_buffer_destroy(bar->buf[1]); bar->buf[1] = NULL; }
    if (bar->shm_data) { munmap(bar->shm_data, (size_t)bar->buf_size * 2); bar->shm_data = NULL; }
    if (bar->shm_pool) { wl_shm_pool_destroy(bar->shm_pool); bar->shm_pool = NULL; }
    if (bar->shm_fd >= 0) { close(bar->shm_fd); bar->shm_fd = -1; }

    int stride = w * 4;  /* ARGB32 */
    int size   = stride * h;

    bar->shm_fd = create_shm_file((size_t)size * 2);
    if (bar->shm_fd < 0) { LOG_ERR("surface: cannot create shm"); exit(1); }

    bar->shm_data = mmap(NULL, (size_t)size * 2,
                         PROT_READ | PROT_WRITE, MAP_SHARED, bar->shm_fd, 0);
    if (bar->shm_data == MAP_FAILED) {
        LOG_ERR("surface: mmap failed: %m");
        exit(1);
    }

    bar->shm_pool = wl_shm_create_pool(bar->shm, bar->shm_fd, size * 2);
    bar->buf[0]   = wl_shm_pool_create_buffer(bar->shm_pool, 0,
                        w, h, stride, WL_SHM_FORMAT_ARGB8888);
    bar->buf[1]   = wl_shm_pool_create_buffer(bar->shm_pool, size,
                        w, h, stride, WL_SHM_FORMAT_ARGB8888);

    bar->buf_width  = w;
    bar->buf_height = h;
    bar->buf_stride = stride;
    bar->buf_size   = size;
    bar->cur_buf    = 0;

    LOG_INFO("surface: resized to %dx%d (stride=%d, pool=%d*2)", w, h, stride, size);
}

/* ------------------------------------------------------------------ */
/* Public: commit current buffer                                        */
/* ------------------------------------------------------------------ */

void bar_surface_commit(void) {
    struct BarState *bar = &g_bar;
    if (!bar->wl_surface || !bar->buf[bar->cur_buf]) return;

    wl_surface_attach(bar->wl_surface, bar->buf[bar->cur_buf], 0, 0);
    wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->buf_width, bar->buf_height);
    wl_surface_commit(bar->wl_surface);

    /* Swap to next buffer */
    bar->cur_buf = 1 - bar->cur_buf;
}

/* ------------------------------------------------------------------ */
/* Public: get pointer to current draw buffer                           */
/* ------------------------------------------------------------------ */

void *bar_surface_get_draw_data(void) {
    struct BarState *bar = &g_bar;
    if (!bar->shm_data) return NULL;
    return (char *)bar->shm_data + (size_t)bar->cur_buf * (size_t)bar->buf_size;
}
