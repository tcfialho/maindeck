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

static void bar_surface_free_buffers(struct BarState *bar) {
    if (bar->buf[0]) { wl_buffer_destroy(bar->buf[0]); bar->buf[0] = NULL; }
    if (bar->buf[1]) { wl_buffer_destroy(bar->buf[1]); bar->buf[1] = NULL; }
    if (bar->shm_data) { munmap(bar->shm_data, (size_t)bar->buf_size * 2); bar->shm_data = NULL; }
    if (bar->shm_pool) { wl_shm_pool_destroy(bar->shm_pool); bar->shm_pool = NULL; }
    if (bar->shm_fd >= 0) { close(bar->shm_fd); bar->shm_fd = -1; }
}

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
        bar_request_redraw(bar);
    }
    bar->configured = true;
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
/* Background surface (Fuzzel empty space click-to-dismiss)             */
/* ------------------------------------------------------------------ */

static int bg_shm_fd = -1;
static void *bg_shm_data = NULL;
static struct wl_shm_pool *bg_shm_pool = NULL;
static size_t bg_shm_size = 0;

static void bg_layer_surface_configure(void *data,
    struct zwlr_layer_surface_v1 *surf,
    uint32_t serial, uint32_t w, uint32_t h)
{
    (void)data;
    struct BarState *bar = &g_bar;
    zwlr_layer_surface_v1_ack_configure(surf, serial);

    int width = (int)w;
    int height = (int)h;
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;

    bar->bg_width = width;
    bar->bg_height = height;

    /* Buffer setup */
    if (bar->viewporter) {
        if (!bar->bg_viewport) {
            bar->bg_viewport = wp_viewporter_get_viewport(bar->viewporter, bar->bg_surface);
        }
        if (bar->bg_viewport) {
            wp_viewport_set_destination(bar->bg_viewport, width, height);
        }
    } else {
        /* Fallback: allocate transparent buffer at screen resolution */
        int stride = width * 4;
        size_t needed_size = (size_t)stride * height;
        if (bar->bg_buffer && bg_shm_size != needed_size) {
            /* Resolution changed, destroy the old fallback buffer first */
            wl_buffer_destroy(bar->bg_buffer);
            bar->bg_buffer = NULL;
            if (bg_shm_data && bg_shm_data != MAP_FAILED) {
                munmap(bg_shm_data, bg_shm_size);
                bg_shm_data = NULL;
            }
            if (bg_shm_pool) {
                wl_shm_pool_destroy(bg_shm_pool);
                bg_shm_pool = NULL;
            }
            if (bg_shm_fd >= 0) {
                close(bg_shm_fd);
                bg_shm_fd = -1;
            }
            bg_shm_size = 0;
        }

        if (!bar->bg_buffer) {
            bg_shm_size = needed_size;
            bg_shm_fd = create_shm_file(bg_shm_size);
            if (bg_shm_fd >= 0) {
                bg_shm_data = mmap(NULL, bg_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, bg_shm_fd, 0);
                if (bg_shm_data != MAP_FAILED) {
                    memset(bg_shm_data, 0, bg_shm_size);
                    bg_shm_pool = wl_shm_create_pool(bar->shm, bg_shm_fd, (int)bg_shm_size);
                    bar->bg_buffer = wl_shm_pool_create_buffer(bg_shm_pool, 0,
                                        width, height, stride, WL_SHM_FORMAT_ARGB8888);
                    bar->bg_buffer_is_fullsize = true;
                } else {
                    LOG_ERR("bg_surface: fallback mmap failed");
                    close(bg_shm_fd);
                    bg_shm_fd = -1;
                    bg_shm_size = 0;
                }
            } else {
                LOG_ERR("bg_surface: fallback create_shm_file failed");
                bg_shm_size = 0;
            }
        }
    }

    if (bar->bg_buffer) {
        wl_surface_attach(bar->bg_surface, bar->bg_buffer, 0, 0);
    }

    wl_surface_set_opaque_region(bar->bg_surface, NULL);

    /* Set input region to capture clicks across the background */
    struct wl_region *region = wl_compositor_create_region(bar->compositor);
    if (region) {
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_input_region(bar->bg_surface, region);
        wl_region_destroy(region);
    }

    if (bar->bg_buffer_is_fullsize) {
        wl_surface_damage_buffer(bar->bg_surface, 0, 0, width, height);
    } else {
        wl_surface_damage_buffer(bar->bg_surface, 0, 0, 1, 1);
    }
    wl_surface_commit(bar->bg_surface);
}

static void bg_layer_surface_closed(void *data,
    struct zwlr_layer_surface_v1 *surf)
{
    (void)data; (void)surf;
    LOG_INFO("bg_surface: background layer surface closed");
}

static const struct zwlr_layer_surface_v1_listener bg_layer_listener = {
    .configure = bg_layer_surface_configure,
    .closed    = bg_layer_surface_closed,
};

void bg_surface_create(void) {
    struct BarState *bar = &g_bar;

    bar->bg_surface = wl_compositor_create_surface(bar->compositor);
    if (!bar->bg_surface) {
        LOG_ERR("bg_surface: wl_compositor_create_surface failed");
        return;
    }

    bar->bg_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        bar->layer_shell,
        bar->bg_surface,
        NULL,   /* output = NULL -> compositor picks */
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "maindeck-bg"
    );
    if (!bar->bg_layer_surface) {
        LOG_ERR("bg_surface: get_layer_surface failed");
        wl_surface_destroy(bar->bg_surface);
        bar->bg_surface = NULL;
        return;
    }

    zwlr_layer_surface_v1_set_anchor(bar->bg_layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    zwlr_layer_surface_v1_set_exclusive_zone(bar->bg_layer_surface, -1);
    zwlr_layer_surface_v1_set_size(bar->bg_layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(bar->bg_layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);

    zwlr_layer_surface_v1_add_listener(bar->bg_layer_surface,
        &bg_layer_listener, NULL);

    /* For viewporter, create the static 1x1 transparent buffer at init time */
    if (bar->viewporter) {
        int fd = create_shm_file(4);
        if (fd >= 0) {
            void *data = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (data != MAP_FAILED) {
                memset(data, 0, 4);
                struct wl_shm_pool *pool = wl_shm_create_pool(bar->shm, fd, 4);
                bar->bg_buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
                bar->bg_buffer_is_fullsize = false;

                bg_shm_fd = fd;
                bg_shm_data = data;
                bg_shm_pool = pool;
                bg_shm_size = 4;
            } else {
                LOG_ERR("bg_surface: mmap failed for 1x1 buffer");
                close(fd);
            }
        } else {
            LOG_ERR("bg_surface: create_shm_file failed for 1x1 buffer");
        }
        if (!bar->bg_buffer) {
            LOG_WARN("bg_surface: failed to allocate 1x1 buffer, viewporter path will fail");
        }
    }

    wl_surface_commit(bar->bg_surface);
    LOG_INFO("bg_surface: background layer surface created (viewporter=%d)", bar->viewporter != NULL);
}

void bg_surface_cleanup(void) {
    struct BarState *bar = &g_bar;

    if (bar->bg_viewport) {
        wp_viewport_destroy(bar->bg_viewport);
        bar->bg_viewport = NULL;
    }
    if (bar->bg_layer_surface) {
        zwlr_layer_surface_v1_destroy(bar->bg_layer_surface);
        bar->bg_layer_surface = NULL;
    }
    if (bar->bg_surface) {
        wl_surface_destroy(bar->bg_surface);
        bar->bg_surface = NULL;
    }
    if (bar->bg_buffer) {
        wl_buffer_destroy(bar->bg_buffer);
        bar->bg_buffer = NULL;
    }

    if (bg_shm_data && bg_shm_data != MAP_FAILED) {
        munmap(bg_shm_data, bg_shm_size);
        bg_shm_data = NULL;
    }
    if (bg_shm_pool) {
        wl_shm_pool_destroy(bg_shm_pool);
        bg_shm_pool = NULL;
    }
    if (bg_shm_fd >= 0) {
        close(bg_shm_fd);
        bg_shm_fd = -1;
    }
    if (bar->viewporter) {
        wp_viewporter_destroy(bar->viewporter);
        bar->viewporter = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Public: destroy / restore bar surface for fullscreen scanout         */
/* ------------------------------------------------------------------ */

void bar_surface_destroy(void) {
    struct BarState *bar = &g_bar;
    if (!bar->wl_surface) return;

    bar_surface_free_buffers(bar);

    if (bar->layer_surface) {
        zwlr_layer_surface_v1_destroy(bar->layer_surface);
        bar->layer_surface = NULL;
    }
    wl_surface_destroy(bar->wl_surface);
    bar->wl_surface = NULL;

    bar->configured = false;
    LOG_INFO("surface: destroyed for fullscreen scanout");
}

void bar_surface_restore(void) {
    struct BarState *bar = &g_bar;
    if (bar->wl_surface) return;

    bar_surface_create();
    wl_display_flush(bar->display);
    LOG_INFO("surface: restored after fullscreen");
}

/* ------------------------------------------------------------------ */
/* Public: resize / allocate buffers                                    */
/* ------------------------------------------------------------------ */

void bar_surface_resize(int w, int h) {
    struct BarState *bar = &g_bar;

    bar_surface_free_buffers(bar);

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
