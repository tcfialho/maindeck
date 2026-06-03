/*
 * maindeck-proxy — Wayland proxy that adds missing output_enter events for River.
 *
 * River's zwlr_foreign_toplevel_manager_v1 never emits output_enter.
 * This proxy sits between any Wayland client (e.g. waybar) and River:
 *
 *   client  ←→  maindeck-proxy  ←→  River
 *
 * All protocols are relayed verbatim — including file descriptor passing via
 * sendmsg/recvmsg with SCM_RIGHTS — EXCEPT zwlr_foreign_toplevel_manager_v1,
 * which the proxy handles itself, adding synthetic output_enter events.
 *
 * Usage in River init:
 *   maindeck-proxy 2>>session.log &
 *   # poll until socket appears
 *   for i in $(seq 20); do
 *       [ -S "$XDG_RUNTIME_DIR/maindeck-0" ] && break; sleep 0.1; done
 *   WAYLAND_DISPLAY=maindeck-0 waybar &
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/wait.h>

#include <wayland-client.h>
#include <time.h>
#include <stdarg.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "proxy-log.h"
#include "proxy-types.h"
#include "proxy-state.h"
#include "proxy-wire.h"
#include "proxy-emit.h"
#include "proxy-toplevel.h"
#include "proxy-relay.h"

bool opt_synthesize = false;    /* MAINDECK_SYNTHESIZE=1: re-enable legacy synthetic toplevels */

bool connect_to_river(int *fd_out) {
    const char *dir  = getenv("XDG_RUNTIME_DIR");
    const char *disp = getenv("WAYLAND_DISPLAY");
    if (!dir || !disp) {
        plog("connect_to_river: missing env XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s",
             dir ? dir : "(null)", disp ? disp : "(null)");
        return false;
    }

    char path[256];
    if (disp[0] == '/')
        snprintf(path, sizeof(path), "%s", disp);
    else
        snprintf(path, sizeof(path), "%s/%s", dir, disp);

    int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (fd < 0) { plog_err("connect_to_river: socket() failed errno=%d", errno); return false; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    /* snprintf into sun_path (always NUL-terminates) and fail on truncation —
     * a silently truncated path would connect to the wrong socket. */
    if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path)
            >= sizeof(addr.sun_path)) {
        plog_err("connect_to_river: path too long for sun_path: %s", path);
        close(fd); return false;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        plog_err("connect_to_river: connect(%s) failed errno=%d (%s)", path, errno, strerror(errno));
        close(fd); return false;
    }
    *fd_out = fd;
    return true;
}

/* ── libwayland event loop thread ──────────────────────────────── */

struct wl_display *proxy_display = NULL;

static void *wayland_thread(void *arg) {
    (void)arg;
    while (wl_display_dispatch(proxy_display) != -1) {}
    return NULL;
}

/* ── River readiness probe ─────────────────────────────────────── */

/*
 * At session start, River accepts new client connections but closes them
 * within ~7ms for ~13 seconds — likely a session/DRM transition during which
 * libseat is still handing off the seat from SDDM. Existing connections
 * (like the proxy's own proxy_display) survive, but new ones don't.
 *
 * If we expose maindeck-0 before River settles, waybar gets ~22 failed
 * connection attempts in a row before one finally sticks, giving the user
 * a 20s black screen.
 *
 * Workaround: open a probe connection, send get_registry, see if it stays
 * alive for 600ms. Retry until it does. THEN create maindeck-0.
 */
static bool river_probe_stable(void) {
    int fd;
    if (!connect_to_river(&fd)) return false;

    /* Send wl_display.get_registry(new_id=2) — same first message waybar
     * would send. If River is going to close us, it does it after we send
     * something, not on bare connection. */
    uint8_t getreg[12];
    wu32(getreg+0, 1);              /* wl_display */
    wu32(getreg+4, (12u<<16)|1u);   /* opcode 1 = get_registry, size 12 */
    wu32(getreg+8, 2);              /* new_id */
    if (send(fd, getreg, sizeof(getreg), MSG_NOSIGNAL) < 0) {
        close(fd);
        return false;
    }

    /* Poll for 600ms. If River sends EOF in that window, it's not stable
     * yet. If it stays open, we consider it stable. */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int remaining_ms = 600;
    while (remaining_ms > 0) {
        int slice = remaining_ms < 100 ? remaining_ms : 100;
        int r = poll(&pfd, 1, slice);
        if (r < 0) { close(fd); return false; }
        if (r > 0) {
            if (pfd.revents & (POLLHUP|POLLERR|POLLNVAL)) {
                close(fd); return false;
            }
            if (pfd.revents & POLLIN) {
                /* Drain available bytes; treat EOF (0) as not-stable */
                uint8_t buf[4096];
                ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n == 0) { close(fd); return false; }
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(fd); return false;
                }
            }
        }
        remaining_ms -= slice;
    }
    close(fd);
    return true;
}

static void wait_for_river_stable(void) {
    plog("probing River stability before exposing maindeck-0");
    int attempt = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!river_probe_stable()) {
        attempt++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
            + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (attempt % 5 == 0) {
            plog("River still unstable after %d attempts (%ldms elapsed)",
                 attempt, elapsed_ms);
        }
        if (elapsed_ms > 60000) {
            plog("River readiness probe gave up after 60s — proceeding anyway");
            return;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 300 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    struct timespec done;
    clock_gettime(CLOCK_MONOTONIC, &done);
    long elapsed_ms = (done.tv_sec - start.tv_sec) * 1000L
        + (done.tv_nsec - start.tv_nsec) / 1000000L;
    plog("River stable after %d attempts (%ldms)", attempt + 1, elapsed_ms);
}

/* ── Server socket ─────────────────────────────────────────────── */

static int create_server_socket(char *out_name, size_t cap) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir) return -1;
	for (int i = 0; i < 32; i++) {
		snprintf(out_name, cap, "maindeck-%d", i);
		char path[256];
		snprintf(path, sizeof(path), "%s/%s", dir, out_name);

		struct sockaddr_un addr = { .sun_family = AF_UNIX };
		/* snprintf (always NUL-terminates) + fail on truncation: a too-long
		 * path can't bind correctly, and the length is index-independent. */
		if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path)
				>= sizeof(addr.sun_path)) {
			plog_err("create_server_socket: path too long for sun_path: %s", path);
			return -1;
		}

		int probe = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
		if (probe >= 0) {
			if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				close(probe);
				continue;
			}
			close(probe);
			if (errno == ECONNREFUSED || errno == ENOENT) {
				unlink(path);
			}
		}

		int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
		if (fd < 0) return -1;
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			listen(fd, 8);
			return fd;
		}
        close(fd);
    }
    return -1;
}

/* ── Master-Worker helper functions ────────────────────────────── */

static int send_fd(int sock_fd, int fd_to_send) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char dummy = 'x';
    iov[0].iov_base = &dummy;
    iov[0].iov_len = 1;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd_to_send;

    if (sendmsg(sock_fd, &msg, 0) < 0) {
        return -1;
    }
    return 0;
}

static int recv_fd(int sock_fd) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char dummy;
    iov[0].iov_base = &dummy;
    iov[0].iov_len = 1;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    if (recvmsg(sock_fd, &msg, 0) < 0) {
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        return -1;
    }

    return *((int *)CMSG_DATA(cmsg));
}

static volatile sig_atomic_t child_exited = 0;
static void handle_sigchld(int sig) {
    (void)sig;
    child_exited = 1;
}

static pid_t spawn_worker(int *control_fd) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        plog_err("Master: socketpair failed: %s", strerror(errno));
        return -1;
    }

    fcntl(pair[1], F_SETFD, 0);

    pid_t pid = fork();
    if (pid < 0) {
        plog_err("Master: fork failed: %s", strerror(errno));
        close(pair[0]);
        close(pair[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process (Worker) */
        close(pair[0]);
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", pair[1]);
        
        char *const argv[] = { "/proc/self/exe", "--worker", fd_str, NULL };
        execv("/proc/self/exe", argv);
        
        /* Fallbacks */
        execv("./build/maindeck-proxy", argv);
        execv("maindeck-proxy", argv);
        
        plog_err("Child: exec failed: %s", strerror(errno));
        exit(1);
    }

    /* Parent process (Master) */
    close(pair[1]);
    *control_fd = pair[0];
    plog("Master: spawned Worker PID %d", pid);
    return pid;
}

static void run_worker(int control_fd) {
    plog("Worker: starting (control_fd=%d)", control_fd);
    
    wl_list_init(&toplevels);
    wl_list_init(&ext_toplevels);
    memset(clients, 0, sizeof(clients));

    proxy_display = wl_display_connect(NULL);
    if (!proxy_display) {
        plog_err("Worker: cannot connect to Wayland (errno=%d %s)", errno, strerror(errno));
        exit(1);
    }
    plog("Worker: connected to River display, doing first roundtrip");

    struct wl_registry *reg = wl_display_get_registry(proxy_display);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(proxy_display);
    if (!proxy_manager) {
        plog_err("Worker: zwlr_foreign_toplevel_manager_v1 not available");
        exit(1);
    }
    plog("Worker: foreign_toplevel_manager bound, doing second roundtrip for initial toplevels");
    wl_display_roundtrip(proxy_display); /* receive initial toplevels */
    plog("Worker: second roundtrip done");

    pthread_t wl_thr;
    pthread_create(&wl_thr, NULL, wayland_thread, NULL);
    plog("Worker: wayland_thread started, entering FD reception loop");

    while (1) {
        int cfd = recv_fd(control_fd);
        if (cfd < 0) {
            plog_err("Worker: recv_fd returned error (control socket closed), exiting");
            break;
        }
        plog("Worker: received accepted client connection fd=%d", cfd);
        setup_client(cfd);
    }
    
    plog("Worker: exiting");
    exit(0);
}

static void run_master(int srv) {
    plog("Master: starting");

    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    int control_fd = -1;
    pid_t worker_pid = spawn_worker(&control_fd);
    if (worker_pid < 0) {
        plog_err("Master: failed to spawn initial worker, exiting");
        return;
    }

    while (1) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                if (child_exited) {
                    child_exited = 0;
                    int status;
                    pid_t reaped = waitpid(worker_pid, &status, WNOHANG);
                    if (reaped == worker_pid) {
                        plog("Master: Worker PID %d exited with status %d, restarting...", worker_pid, status);
                        close(control_fd);
                        worker_pid = spawn_worker(&control_fd);
                        if (worker_pid < 0) {
                            plog_err("Master: failed to restart worker, exiting");
                            break;
                        }
                    }
                }
                continue;
            }
            plog_err("Master: accept failed: %s", strerror(errno));
            break;
        }

        plog("Master: accepted new client connection cfd=%d, handing off to Worker", cfd);
        if (send_fd(control_fd, cfd) < 0) {
            plog_err("Master: failed to send fd to Worker, closing cfd");
        }
        close(cfd);
    }
}

/* ── main ──────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Read runtime toggles from env (inherited across the worker re-exec).
     * Default is Direction B (no synthesis). MAINDECK_SYNTHESIZE=1 opts back
     * into the legacy synthetic-handle path for A/B comparison. */
    const char *syn = getenv("MAINDECK_SYNTHESIZE");
    opt_synthesize = (syn && syn[0] && syn[0] != '0');

    if (argc >= 3 && strcmp(argv[1], "--worker") == 0) {
        int control_fd = atoi(argv[2]);
        plog("Worker: mode=%s", opt_synthesize ? "LEGACY-SYNTHESIS" : "DIRECTION-B (forward real handles + inject output_enter)");
        run_worker(control_fd);
        return 0;
    }

    plog("Master starting (WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s synthesize=%d)",
         getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(null)",
         getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(null)",
         opt_synthesize);

    wait_for_river_stable();

    char name[64];
    int srv = create_server_socket(name, sizeof(name));
    if (srv < 0) {
        plog_err("Master: cannot create server socket");
        return 1;
    }

    /* Print socket name so the init script can use it */
    printf("%s\n", name);
    fflush(stdout);
    plog("Master listening on %s/%s",
         getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "?", name);

    run_master(srv);

    close(srv);
    return 0;
}
