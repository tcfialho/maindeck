#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <malloc.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <sys/file.h>
#include <fcntl.h>

#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "bar-state.h"
#include "bar-surface.h"
#include "bar-render.h"
#include "bar-taskbar.h"
#include "bar-input.h"
#include "bar-status.h"
#include "bar-tray.h"
#include "bar-config.h"
#include "bar-icons.h"
#include "bar-log.h"
#include "bar-game-mode.h"

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

struct BarState g_bar;

/* ------------------------------------------------------------------ */
/* Registry                                                             */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
    struct BarState *bar = &g_bar;

    if (strcmp(iface, "wl_compositor") == 0) {
        bar->compositor = wl_registry_bind(reg, name,
            &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(iface, "wl_shm") == 0) {
        bar->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, "wl_seat") == 0) {
        bar->seat = wl_registry_bind(reg, name, &wl_seat_interface,
            version < 7 ? version : 7);
    } else if (strcmp(iface, "wl_output") == 0 && !bar->output) {
        bar->output = wl_registry_bind(reg, name, &wl_output_interface,
            version < 4 ? version : 4);
    } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
        bar->layer_shell = wl_registry_bind(reg, name,
            &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    } else if (strcmp(iface, "zwlr_foreign_toplevel_manager_v1") == 0) {
        bar->toplevel_mgr = wl_registry_bind(reg, name,
            &zwlr_foreign_toplevel_manager_v1_interface,
            version < 3 ? version : 3);
    } else if (strcmp(iface, "ext_foreign_toplevel_list_v1") == 0) {
        bar->ext_list = wl_registry_bind(reg, name,
            &ext_foreign_toplevel_list_v1_interface, 1);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        bar->xdg_wm_base = wl_registry_bind(reg, name,
            &xdg_wm_base_interface, 1);
    } else if (strcmp(iface, "wp_viewporter") == 0) {
        bar->viewporter = wl_registry_bind(reg, name,
            &wp_viewporter_interface, 1);
    } else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        bar->cursor_shape_manager = wl_registry_bind(reg, name,
            &wp_cursor_shape_manager_v1_interface, version < 1 ? version : 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
    uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

/* ------------------------------------------------------------------ */
/* Seat: get pointer on capability                                      */
/* ------------------------------------------------------------------ */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    struct BarState *bar = &g_bar;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !bar->pointer) {
        bar->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(bar->pointer, &bar_pointer_listener, NULL);
        if (bar->cursor_shape_manager) {
            bar->cursor_shape_device =
                wp_cursor_shape_manager_v1_get_pointer(bar->cursor_shape_manager, bar->pointer);
        }
        LOG_INFO("main: pointer acquired");
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && bar->pointer) {
        if (bar->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(bar->cursor_shape_device);
            bar->cursor_shape_device = NULL;
        }
        wl_pointer_destroy(bar->pointer);
        bar->pointer = NULL;
        bar->ptr_surface = NULL;
        bar->ptr_inside = false;
        bar->ptr_on_menu = false;
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *n) {
    (void)d; (void)s; (void)n;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* ------------------------------------------------------------------ */
/* IPC socket setup                                                     */
/* ------------------------------------------------------------------ */

static void ipc_init(void) {
    struct BarState *bar = &g_bar;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) { bar->ipc_sock = -1; bar->notify_sock = -1; return; }

    if ((size_t)snprintf(bar->ipc_path, sizeof(bar->ipc_path),
            "%s/maindeck-wm.sock", dir) >= sizeof(bar->ipc_path)) {
        bar->ipc_sock = -1;
        bar->notify_sock = -1;
        return;
    }

    bar->ipc_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (bar->ipc_sock < 0) {
        LOG_WARN("main: IPC socket creation failed: %s", strerror(errno));
    }

    /* Notify socket: WM sends fullscreen_on/fullscreen_off here */
    char notify_path[108];
    if ((size_t)snprintf(notify_path, sizeof(notify_path),
            "%s/maindeck-bar.sock", dir) < sizeof(notify_path)) {
        int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd >= 0) {
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", notify_path);
            unlink(notify_path);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                bar->notify_sock = fd;
                LOG_INFO("main: notify socket bound at %s", notify_path);
            } else {
                LOG_WARN("main: notify socket bind failed: %s", strerror(errno));
                close(fd);
                bar->notify_sock = -1;
            }
        } else {
            bar->notify_sock = -1;
        }
    } else {
        bar->notify_sock = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                       */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Config path                                                          */
/* ------------------------------------------------------------------ */

static void get_config_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, cap, "%s/.config/maindeck/bar.json", home);
    } else {
        snprintf(out, cap, "/etc/maindeck/bar.json");
    }
}

/* ------------------------------------------------------------------ */
/* Timer: next minute boundary                                          */
/* ------------------------------------------------------------------ */

static int ms_until_next_minute(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int sec_in_min = (int)(ts.tv_sec % 60);
    int ms_left = (60 - sec_in_min) * 1000 - (int)(ts.tv_nsec / 1000000);
    if (ms_left <= 0) ms_left = 60000;
    return ms_left;
}

/* ------------------------------------------------------------------ */
/* Singleton lock                                                       */
/* ------------------------------------------------------------------ */

static int comm_matches(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    size_t len = strcspn(buf, "\n");
    buf[len] = '\0';
    return strcmp(buf, "maindeck-bar") == 0;
}

static int singleton_acquire(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) {
        LOG_WARN("singleton: XDG_RUNTIME_DIR not set, skipping singleton check");
        return -1;
    }
    char path[256];
    if ((size_t)snprintf(path, sizeof(path), "%s/maindeck-bar.lock", dir) >= sizeof(path)) {
        LOG_WARN("singleton: path too long");
        return -1;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARN("singleton: failed to open lockfile %s: %s", path, strerror(errno));
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        goto own;
    }

    if (errno == EWOULDBLOCK) {
        char buf[32];
        ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            long pid_long = strtol(buf, NULL, 10);
            if (pid_long > 0) {
                pid_t old_pid = (pid_t)pid_long;
                if (kill(old_pid, 0) == 0 && comm_matches(old_pid)) {
                    LOG_INFO("singleton: sending SIGTERM to existing instance (PID %d)", (int)old_pid);
                    kill(old_pid, SIGTERM);
                }
            }
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        for (int i = 0; i < 40; i++) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                goto own;
            }
            nanosleep(&ts, NULL);
        }

        LOG_WARN("singleton: timeout waiting for lock, proceeding anyway");
        goto own;
    }

    LOG_WARN("singleton: flock failed: %s", strerror(errno));
    close(fd);
    return -1;

own:
    if (ftruncate(fd, 0) == 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
        if (n > 0) {
            (void)pwrite(fd, buf, n, 0);
        }
    } else {
        LOG_WARN("singleton: ftruncate failed: %s", strerror(errno));
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */


int main(void) {
#ifdef __GLIBC__
    mallopt(M_ARENA_MAX, 1);
#endif
    /* Cap stack hard limit so glibc doesn't reserve terabytes of virtual address
     * space for the main-stack guard and heap arenas. Without this, inheriting
     * RLIM_INFINITY from the parent shell causes ~20TB VmSize at startup. */
    struct rlimit stk = { 2 * 1024 * 1024, 2 * 1024 * 1024 };
    setrlimit(RLIMIT_STACK, &stk);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGCHLD, SIG_IGN);  /* reap children automatically */

    /* Acquire singleton lock to prevent multiple concurrent instances */
    int lock_fd = singleton_acquire();
    (void)lock_fd; /* keep fd open until process exits to hold the lock */

    memset(&g_bar, 0, sizeof(g_bar));
    g_bar.shm_fd     = -1;
    g_bar.ipc_sock   = -1;
    g_bar.notify_sock = -1;
    g_bar.hover_hit  = -1;
    g_bar.hover_type    = HIT_NONE;
    g_bar.hover_index   = -1;
    g_bar.menu_hover_row = -1;
    g_bar.height    = 32;
    bar_game_mode_reset_notifications();

    /* Load config */
    char cfg_path[256];
    get_config_path(cfg_path, sizeof(cfg_path));
    bar_config_load(cfg_path, &g_bar.config);
    g_bar.height = g_bar.config.height > 0 ? g_bar.config.height : 32;

    /* Connect to Wayland */
    g_bar.display = wl_display_connect(NULL);
    if (!g_bar.display) {
        LOG_ERR("main: cannot connect to Wayland display");
        return 1;
    }
    LOG_INFO("main: connected to %s", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?");

    g_bar.registry = wl_display_get_registry(g_bar.display);
    wl_registry_add_listener(g_bar.registry, &registry_listener, NULL);
    wl_display_roundtrip(g_bar.display);

    /* Verify required globals */
    if (!g_bar.compositor || !g_bar.shm || !g_bar.layer_shell) {
        LOG_ERR("main: missing required Wayland globals (compositor=%p shm=%p layer_shell=%p)",
            (void*)g_bar.compositor, (void*)g_bar.shm, (void*)g_bar.layer_shell);
        return 1;
    }
    if (!g_bar.toplevel_mgr) {
        LOG_WARN("main: zwlr_foreign_toplevel_manager_v1 not available — taskbar disabled");
    }

    /* Seat (pointer) */
    if (g_bar.seat) {
        wl_seat_add_listener(g_bar.seat, &seat_listener, NULL);
    }

    /* xdg_wm_base ping */
    if (g_bar.xdg_wm_base) {
        static const struct xdg_wm_base_listener wm_base_listener = {
            .ping = xdg_wm_base_ping,
        };
        xdg_wm_base_add_listener(g_bar.xdg_wm_base, &wm_base_listener, NULL);
    }

    /* Foreign toplevel listeners */
    if (g_bar.toplevel_mgr) {
        zwlr_foreign_toplevel_manager_v1_add_listener(
            g_bar.toplevel_mgr, &bar_mgr_listener, NULL);
    }
    if (g_bar.ext_list) {
        ext_foreign_toplevel_list_v1_add_listener(
            g_bar.ext_list, &bar_ext_list_listener, NULL);
    }

    /* IPC socket */
    ipc_init();

    /* Create layer surface */
    bar_surface_create();
    bg_surface_create();

    /* Second roundtrip: receive configure + initial toplevels */
    wl_display_roundtrip(g_bar.display);
    wl_display_roundtrip(g_bar.display);

    /* Status modules */
    bar_status_init();

    /* System tray */
    int tray_fd = bar_tray_init();

    /* Initial render */
    bar_status_tick();
    if (g_bar.dirty) bar_render();

    int wl_fd    = wl_display_get_fd(g_bar.display);
    int timer_ms = ms_until_next_minute();

    LOG_INFO("main: entering event loop (tray_fd=%d)", tray_fd);

    while (g_running) {
        while (wl_display_prepare_read(g_bar.display) != 0)
            wl_display_dispatch_pending(g_bar.display);

        if (wl_display_flush(g_bar.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g_bar.display);
            break;
        }

        int notify_fd = g_bar.notify_sock;
        struct pollfd pfds[3] = {
            { .fd = wl_fd,     .events = POLLIN },
            { .fd = tray_fd,   .events = tray_fd >= 0   ? POLLIN : 0 },
            { .fd = notify_fd, .events = notify_fd >= 0 ? POLLIN : 0 },
        };
        nfds_t nfds = 3;

        int ret = poll(pfds, nfds, timer_ms);

        if (ret < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(g_bar.display);
                continue;
            }
            wl_display_cancel_read(g_bar.display);
            break;
        }

        if (ret == 0) {
            /* Timer fired — minute tick */
            wl_display_cancel_read(g_bar.display);
            bar_status_tick();
            bar_taskbar_prune_ghosts(); /* fantasmas nascidos com a sessão ociosa */
            timer_ms = ms_until_next_minute();
        } else {
            if (pfds[0].revents & POLLIN) {
                wl_display_read_events(g_bar.display);
                wl_display_dispatch_pending(g_bar.display);
            } else {
                wl_display_cancel_read(g_bar.display);
            }
            if (tray_fd >= 0 && (pfds[1].revents & POLLIN))
                bar_tray_dispatch();
            if (notify_fd >= 0 && (pfds[2].revents & POLLIN)) {
                char buf[4096];
                ssize_t n = recv(notify_fd, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    if (strncmp(buf, "windows", 7) == 0 &&
                        (buf[7] == '\0' || buf[7] == ' ' || buf[7] == '\n')) {
                        /* Conjunto de janelas vivas do WM → poda fantasmas */
                        bar_taskbar_set_wm_windows(buf);
                    } else {
                        bool on = (strncmp(buf, "fullscreen_on", 13) == 0);
                        bool off = (strncmp(buf, "fullscreen_off", 14) == 0);
                        LOG_INFO("main: notify received: %s", buf);
                        g_bar.wm_fullscreen = on;
                        if ((on || off) && g_bar.render_suppressed != on) {
                            g_bar.render_suppressed = on;
                            bar_game_mode_apply(on);
                            if (!on) {
                                g_bar.dirty_deferred = false;
                                bar_surface_restore();
                            } else {
                                g_bar.dirty = false;
                                g_bar.dirty_deferred = true;
                                bar_surface_destroy();
                            }
                        }
                    }
                }
            }
            timer_ms = ms_until_next_minute();
        }

        /* Redraw if needed */
        if (g_bar.dirty && g_bar.configured && !g_bar.render_suppressed)
            bar_render();
    }

    LOG_INFO("main: shutting down");
    bg_surface_cleanup();
    bar_tray_cleanup();
    bar_status_cleanup();
    bar_icons_cleanup();
    bar_render_cleanup();
    if (g_bar.cursor_shape_device) wp_cursor_shape_device_v1_destroy(g_bar.cursor_shape_device);
    if (g_bar.pointer) wl_pointer_destroy(g_bar.pointer);
    if (g_bar.cursor_shape_manager) wp_cursor_shape_manager_v1_destroy(g_bar.cursor_shape_manager);
    if (g_bar.ipc_sock >= 0) close(g_bar.ipc_sock);
    if (g_bar.notify_sock >= 0) close(g_bar.notify_sock);
    wl_display_disconnect(g_bar.display);
    return 0;
}
