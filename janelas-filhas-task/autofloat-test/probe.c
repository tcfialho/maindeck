#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <wayland-client.h>
#include "xdg-shell-protocol.h"

struct Client;

struct WindowState {
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_buffer *buffer;
    int width, height;
    bool configured;
    struct Client *client;
};

struct Client {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_shm *shm;
    
    struct WindowState parent;
    struct WindowState child;
    bool has_child;

    const char *mode;
    int target_w, target_h;

    // --slow: segura o 1º configure (ack sem commit); quando o 2º chegar com
    // tamanho diferente (relayout), commita primeiro o tamanho ANTIGO (resposta
    // atrasada à proposta anterior), espera, e então commita o novo.
    int slow_held_w, slow_held_h;
    bool slow_replayed;
};

static int create_shm_file(off_t size) {
    int fd = memfd_create("wayland-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        char name[] = "/tmp/wayland-shm-XXXXXX";
        fd = mkstemp(name);
        if (fd >= 0) {
            unlink(name);
        }
    }
    if (fd >= 0) {
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static struct wl_buffer *create_buffer(struct wl_shm *shm, int width, int height) {
    int stride = width * 4;
    int size = stride * height;
    int fd = create_shm_file(size);
    if (fd < 0) return NULL;
    
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    struct Client *client = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        client->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct WindowState *state = data;
    struct Client *client = state->client;
    
    xdg_surface_ack_configure(xdg_surface, serial);
    
    int w = state->width;
    int h = state->height;
    
    if (strcmp(client->mode, "--normal") == 0) {
        if (w <= 0 || h <= 0) {
            w = 640;
            h = 480;
        }
    } else if (strcmp(client->mode, "--slow") == 0) {
        if (w <= 0 || h <= 0) { w = 640; h = 480; }
        if (client->slow_held_w == 0) {
            // 1º configure: ack já foi enviado acima; segura sem commit.
            client->slow_held_w = w;
            client->slow_held_h = h;
            printf("SLOW HOLD %dx%d\n", w, h);
            fflush(stdout);
            return;
        }
        if (!client->slow_replayed && (w != client->slow_held_w || h != client->slow_held_h)) {
            client->slow_replayed = true;
            // Resposta atrasada: commita o tamanho da proposta ANTERIOR primeiro.
            int old_w = client->slow_held_w, old_h = client->slow_held_h;
            struct wl_buffer *old_buf = create_buffer(client->shm, old_w, old_h);
            if (old_buf) {
                wl_surface_attach(state->surface, old_buf, 0, 0);
                wl_surface_damage(state->surface, 0, 0, old_w, old_h);
                wl_surface_commit(state->surface);
                wl_display_flush(client->display);
                printf("SLOW REPLAY %dx%d\n", old_w, old_h);
                fflush(stdout);
                usleep(400000); // deixa o WM processar dimensions(antigo) entre frames
                wl_buffer_destroy(old_buf);
            }
            // cai para o commit normal com o tamanho novo (w,h) abaixo
        }
    } else if (strcmp(client->mode, "--fixed") == 0) {
        if (w <= 0 || h <= 0) {
            w = client->target_w;
            h = client->target_h;
        }
    } else if (strcmp(client->mode, "--stubborn") == 0) {
        w = client->target_w;
        h = client->target_h;
    } else if (strcmp(client->mode, "--huge") == 0) {
        if (w <= 0 || h <= 0) {
            w = client->target_w;
            h = client->target_h;
        }
    } else if (strcmp(client->mode, "--child") == 0) {
        if (w <= 0 || h <= 0) {
            w = 320;
            h = 240;
        }
    }
    
    if (state->buffer == NULL || state->width != w || state->height != h) {
        if (state->buffer) {
            wl_buffer_destroy(state->buffer);
        }
        state->buffer = create_buffer(client->shm, w, h);
        if (state->buffer) {
            wl_surface_attach(state->surface, state->buffer, 0, 0);
            wl_surface_damage(state->surface, 0, 0, w, h);
        }
    }
    wl_surface_commit(state->surface);
    state->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states) {
    struct WindowState *state = data;
    printf("CONFIGURE %dx%d\n", width, height);
    fflush(stdout);
    
    if (width > 0 && height > 0) {
        state->width = width;
        state->height = height;
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void init_window(struct Client *client, struct WindowState *state, const char *app_id) {
    state->client = client;
    state->width = 0;
    state->height = 0;
    state->configured = false;
    state->buffer = NULL;
    
    state->surface = wl_compositor_create_surface(client->compositor);
    state->xdg_surface = xdg_wm_base_get_xdg_surface(client->xdg_wm_base, state->surface);
    xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener, state);
    
    state->xdg_toplevel = xdg_surface_get_toplevel(state->xdg_surface);
    xdg_toplevel_add_listener(state->xdg_toplevel, &xdg_toplevel_listener, state);
    xdg_toplevel_set_app_id(state->xdg_toplevel, app_id);
    xdg_toplevel_set_title(state->xdg_toplevel, app_id);
    
    if (strcmp(client->mode, "--fixed") == 0) {
        xdg_toplevel_set_min_size(state->xdg_toplevel, client->target_w, client->target_h);
        xdg_toplevel_set_max_size(state->xdg_toplevel, client->target_w, client->target_h);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mode> [WxH]\n", argv[0]);
        return 1;
    }
    
    struct Client client = {0};
    client.mode = argv[1];
    if (argc >= 3) {
        sscanf(argv[2], "%dx%d", &client.target_w, &client.target_h);
    }
    
    client.display = wl_display_connect(NULL);
    if (!client.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }
    
    client.registry = wl_display_get_registry(client.display);
    wl_registry_add_listener(client.registry, &registry_listener, &client);
    
    wl_display_roundtrip(client.display);
    
    if (!client.compositor || !client.xdg_wm_base || !client.shm) {
        fprintf(stderr, "Failed to bind globals\n");
        return 1;
    }
    
    xdg_wm_base_add_listener(client.xdg_wm_base, &xdg_wm_base_listener, &client);
    
    if (strcmp(client.mode, "--child") == 0) {
        // Mapeia o PAI primeiro (fluxo real: dialog abre com a principal já
        // mapeada). O River só resolve .parent com o pai mapeado — set_parent
        // com ambos pré-map nunca gera o evento.
        init_window(&client, &client.parent, "probe-child-parent");
        wl_surface_commit(client.parent.surface);
        while (!client.parent.configured) {
            if (wl_display_dispatch(client.display) < 0) return 1;
        }
        init_window(&client, &client.child, "probe-child");
        xdg_toplevel_set_parent(client.child.xdg_toplevel, client.parent.xdg_toplevel);
        client.has_child = true;
        wl_surface_commit(client.child.surface);
    } else {
        const char *app_id = "probe-normal";
        if (strcmp(client.mode, "--fixed") == 0) app_id = "probe-fixed";
        else if (strcmp(client.mode, "--stubborn") == 0) app_id = "probe-stubborn";
        else if (strcmp(client.mode, "--huge") == 0) app_id = "probe-huge";
        else if (strcmp(client.mode, "--slow") == 0) app_id = "probe-slow";
        
        init_window(&client, &client.parent, app_id);
        wl_surface_commit(client.parent.surface);
    }

    wl_display_roundtrip(client.display);
    
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    while (1) {
        wl_display_dispatch_pending(client.display);
        if (wl_display_flush(client.display) < 0) {
            break;
        }
        
        wl_display_dispatch(client.display);
        
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
        if (elapsed >= 2.0) {
            break;
        }
        
        usleep(10000);
    }
    
    if (client.parent.buffer) wl_buffer_destroy(client.parent.buffer);
    if (client.parent.xdg_toplevel) xdg_toplevel_destroy(client.parent.xdg_toplevel);
    if (client.parent.xdg_surface) xdg_surface_destroy(client.parent.xdg_surface);
    if (client.parent.surface) wl_surface_destroy(client.parent.surface);
    
    if (client.has_child) {
        if (client.child.buffer) wl_buffer_destroy(client.child.buffer);
        if (client.child.xdg_toplevel) xdg_toplevel_destroy(client.child.xdg_toplevel);
        if (client.child.xdg_surface) xdg_surface_destroy(client.child.xdg_surface);
        if (client.child.surface) wl_surface_destroy(client.child.surface);
    }
    
    if (client.xdg_wm_base) xdg_wm_base_destroy(client.xdg_wm_base);
    if (client.shm) wl_shm_destroy(client.shm);
    if (client.compositor) wl_compositor_destroy(client.compositor);
    wl_registry_destroy(client.registry);
    wl_display_disconnect(client.display);
    
    return 0;
}
