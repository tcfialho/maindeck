// SPDX-FileCopyrightText: © 2026 Isaac Freund
// SPDX-License-Identifier: 0BSD

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <ctype.h>

#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <river-input-management-v1-client-protocol.h>

#include "types.h"
#include "wm-log.h"
#include "wm-state.h"
#include "wm-layout.h"
#include "wm-input.h"
#include "wm-libinput.h"
#include "wm-handlers.h"

struct WindowManager wm;
struct wl_display *wm_display;
struct river_window_manager_v1 *window_manager_v1;
struct river_xkb_bindings_v1 *xkb_bindings_v1;
struct river_layer_shell_v1 *layer_shell_v1;
struct river_libinput_config_v1 *libinput_config_v1;

/* IPC: socket DGRAM que recebe "activate <identifier>" do proxy */
static int  ipc_fd = -1;
static char ipc_path[108]; /* sizeof(sockaddr_un.sun_path) */
/* Compartilhado com wm-layout.c (apply_pending_taskbar_activation) → extern em wm-state.h */
char pending_activate_identifier[33];


static int ipc_init(void) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (dir == NULL || dir[0] == '\0') {
		LOG_WARN("ipc_init: XDG_RUNTIME_DIR não definido");
		return -1;
	}
	if ((size_t)snprintf(ipc_path, sizeof(ipc_path), "%s/maindeck-wm.sock", dir)
			>= sizeof(ipc_path)) {
		LOG_WARN("ipc_init: path muito longo");
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		LOG_WARN("ipc_init: socket() falhou: %s", strerror(errno));
		return -1;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_path)
			>= sizeof(addr.sun_path)) {
		LOG_WARN("ipc_init: path não cabe em sun_path");
		close(fd);
		return -1;
	}
	unlink(ipc_path); /* limpa socket antigo */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_WARN("ipc_init: bind(%s) falhou: %s", ipc_path, strerror(errno));
		close(fd);
		return -1;
	}
	LOG_INFO("IPC socket escutando em %s", ipc_path);
	return fd;
}

static void ipc_handle_message(const char *msg) {
	const char prefix[] = "activate ";
	const size_t prefix_len = sizeof(prefix) - 1;
	if (strncmp(msg, prefix, prefix_len) != 0) {
		LOG_WARN("IPC: comando desconhecido ignorado: %s", msg);
		return;
	}
	const char *id = msg + prefix_len;
	if (!valid_identifier(id)) {
		LOG_WARN("IPC: identifier inválido ignorado: %s", id);
		return;
	}
	LOG_EVENT("IPC recebeu activate identifier=%s", id);
	snprintf(pending_activate_identifier, sizeof(pending_activate_identifier), "%s", id);
	/* Acorda o manage cycle para aplicar o activate imediatamente */
	river_window_manager_v1_manage_dirty(window_manager_v1);
}

static void ipc_drain(void) {
	if (ipc_fd < 0) return;
	for (;;) {
		char buf[128];
		ssize_t n = recv(ipc_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return;
			LOG_WARN("IPC recv falhou: %s", strerror(errno));
			return;
		}
		if (n == 0) return;
		buf[n] = '\0';
		char *nl = strchr(buf, '\n');
		if (nl != NULL) *nl = '\0';
		ipc_handle_message(buf);
	}
}

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to connect to Wayland server\n");
		return 1;
	}
	wm_display = display;

	log_init();
	LOG_INFO("maindeck-wm starting");
	unsetenv("WAYLAND_DEBUG");
	signal(SIGCHLD, SIG_IGN);

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "roundtrip failed\n");
		return 1;
	}

	if (window_manager_v1 == NULL || xkb_bindings_v1 == NULL) {
		LOG_WARN("river_window_manager_v1 or river_xkb_bindings_v1 not supported");
		return 1;
	}
	LOG_INFO("globals bound: window_manager=%p xkb_bindings=%p layer_shell=%p libinput_config=%p",
		(void *)window_manager_v1, (void *)xkb_bindings_v1, (void *)layer_shell_v1, (void *)libinput_config_v1);
	if (layer_shell_v1 == NULL) {
		LOG_WARN("river_layer_shell_v1 not advertised — layer-shell clients (waybar) will be closed by the compositor");
	}

	if (libinput_config_v1 != NULL) {
		LOG_INFO("libinput_config_v1 bound: adding listener");
		river_libinput_config_v1_add_listener(libinput_config_v1, &libinput_config_listener, NULL);
	}

	init_libinput_listeners();
	wm_init();
	river_window_manager_v1_add_listener(window_manager_v1, &wm_listener, NULL);

	ipc_fd = ipc_init();

	// Poll-based event loop with hold-timer support
	while (true) {
		// Drain any queued events before sleeping
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0) goto done;
		}

		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(display);
			break;
		}

		int timeout_ms = compute_poll_timeout();
		struct pollfd pfd[2] = {
			{ .fd = wl_display_get_fd(display), .events = POLLIN },
			{ .fd = ipc_fd, .events = ipc_fd >= 0 ? POLLIN : 0 },
		};
		nfds_t nfds = ipc_fd >= 0 ? 2 : 1;
		int ret = poll(pfd, nfds, timeout_ms);

		if (ret < 0) {
			wl_display_cancel_read(display);
			break;
		}

		if (pfd[0].revents & POLLIN) {
			wl_display_read_events(display);
			if (wl_display_dispatch_pending(display) < 0) break;
			if (ipc_fd >= 0 && (pfd[1].revents & POLLIN)) {
				ipc_drain();
			}
		} else if (ipc_fd >= 0 && (pfd[1].revents & POLLIN)) {
			/* Só IPC acordou: cancela o read Wayland e drena o IPC */
			wl_display_cancel_read(display);
			ipc_drain();
			if (wl_display_dispatch_pending(display) < 0) break;
		} else {
			// Timeout: a hold threshold was reached
			wl_display_cancel_read(display);
			process_hold_timers();
			if (wl_display_dispatch_pending(display) < 0) break;
		}
	}

done:
	wm_display = NULL;
	log_close();
	if (ipc_fd >= 0) close(ipc_fd);
	if (ipc_path[0] != '\0') unlink(ipc_path);
	return 0;
}
