#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct Top {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title;
	char *app_id;
	bool activated;
	bool closed;
	int output_enters;
	int done_count;
	struct wl_list link;
};

struct State {
	struct wl_display *display;
	struct wl_registry *registry;
	struct zwlr_foreign_toplevel_manager_v1 *manager;
	struct wl_output *first_output;
	struct wl_seat *first_seat;
	uint32_t first_output_name;
	uint32_t first_seat_name;
	int outputs;
	int seats;
	int foreign_globals;
	struct wl_list tops;
};

static void die_alarm(int sig) {
	(void)sig;
	fprintf(stderr, "timeout waiting for Wayland events\n");
	_exit(124);
}

static void set_str(char **dst, const char *src) {
	free(*dst);
	*dst = src ? strdup(src) : NULL;
}

static void top_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *title) {
	(void)handle;
	set_str(&((struct Top *)data)->title, title);
}

static void top_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *app_id) {
	(void)handle;
	set_str(&((struct Top *)data)->app_id, app_id);
}

static void top_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_output *output) {
	(void)handle;
	(void)output;
	((struct Top *)data)->output_enters++;
}

static void top_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_output *output) {
	(void)handle;
	(void)output;
	struct Top *top = data;
	if (top->output_enters > 0) top->output_enters--;
}

static void top_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_array *state) {
	(void)handle;
	struct Top *top = data;
	top->activated = false;

	uint32_t *value;
	wl_array_for_each(value, state) {
		if (*value == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
			top->activated = true;
		}
	}
}

static void top_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
	(void)handle;
	((struct Top *)data)->done_count++;
}

static void top_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
	(void)handle;
	((struct Top *)data)->closed = true;
}

static void top_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
		struct zwlr_foreign_toplevel_handle_v1 *parent) {
	(void)data;
	(void)handle;
	(void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener top_listener = {
	.title = top_title,
	.app_id = top_app_id,
	.output_enter = top_output_enter,
	.output_leave = top_output_leave,
	.state = top_state,
	.done = top_done,
	.closed = top_closed,
	.parent = top_parent,
};

static void manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
		struct zwlr_foreign_toplevel_handle_v1 *handle) {
	(void)manager;
	struct State *state = data;
	struct Top *top = calloc(1, sizeof(*top));
	if (!top) return;
	top->handle = handle;
	wl_list_insert(state->tops.prev, &top->link);
	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &top_listener, top);
}

static void manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {
	(void)data;
	(void)manager;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
	.toplevel = manager_toplevel,
	.finished = manager_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	struct State *state = data;
	if (strcmp(interface, wl_output_interface.name) == 0) {
		state->outputs++;
		if (!state->first_output) {
			state->first_output_name = name;
			state->first_output = wl_registry_bind(registry, name, &wl_output_interface,
				version < 4 ? version : 4);
		}
	} else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		state->foreign_globals++;
		if (!state->manager) {
			state->manager = wl_registry_bind(registry, name,
				&zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
			zwlr_foreign_toplevel_manager_v1_add_listener(state->manager, &manager_listener, state);
		}
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seats++;
		if (!state->first_seat) {
			state->first_seat_name = name;
			state->first_seat = wl_registry_bind(registry, name, &wl_seat_interface,
				version < 5 ? version : 5);
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void dispatch_for(struct wl_display *display, int ms) {
	int fd = wl_display_get_fd(display);
	int elapsed = 0;

	while (elapsed < ms) {
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0) return;
		}
		if (wl_display_flush(display) < 0) {
			wl_display_cancel_read(display);
			return;
		}

		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int step = 50;
		int ret = poll(&pfd, 1, step);
		elapsed += step;
		if (ret > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_read_events(display) < 0) return;
			if (wl_display_dispatch_pending(display) < 0) return;
		} else {
			wl_display_cancel_read(display);
		}
	}
}

static bool parse_int(const char *s, int *out) {
	char *end = NULL;
	errno = 0;
	long value = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || value < 0 || value > INT32_MAX) {
		return false;
	}
	*out = (int)value;
	return true;
}

static void usage(const char *argv0) {
	fprintf(stderr,
		"usage: %s [--activate-index=N | --activate-app=APP_ID]\n",
		argv0);
}

int main(int argc, char **argv) {
	signal(SIGALRM, die_alarm);
	alarm(5);

	int activate_index = -1;
	const char *activate_app = NULL;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--activate-index=", 17) == 0) {
			if (!parse_int(argv[i] + 17, &activate_index)) {
				usage(argv[0]);
				return 64;
			}
		} else if (strncmp(argv[i], "--activate-app=", 15) == 0) {
			activate_app = argv[i] + 15;
		} else {
			usage(argv[0]);
			return 64;
		}
	}

	struct State state = {0};
	wl_list_init(&state.tops);

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		fprintf(stderr, "cannot connect to Wayland display\n");
		return 1;
	}

	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);

	if (!state.manager) {
		printf("outputs=%d foreign_globals=%d manager=missing\n", state.outputs, state.foreign_globals);
		return 2;
	}

	wl_display_roundtrip(state.display);
	dispatch_for(state.display, 500);

	if (activate_index >= 0 || activate_app != NULL) {
		if (!state.first_seat) {
			fprintf(stderr, "cannot activate: no wl_seat advertised\n");
			return 4;
		}
		int index = 0;
		struct Top *chosen = NULL;
		struct Top *top;
		wl_list_for_each(top, &state.tops, link) {
			if (top->closed) continue;
			if ((activate_index >= 0 && index == activate_index) ||
					(activate_app != NULL && top->app_id != NULL &&
					 strcmp(top->app_id, activate_app) == 0)) {
				chosen = top;
				break;
			}
			index++;
		}
		if (!chosen) {
			fprintf(stderr, "cannot activate: no matching toplevel\n");
			return 5;
		}
		printf("activating index=%d title=\"%s\" app_id=\"%s\" seat_name=%u\n",
			index,
			chosen->title ? chosen->title : "",
			chosen->app_id ? chosen->app_id : "",
			state.first_seat_name);
		zwlr_foreign_toplevel_handle_v1_activate(chosen->handle, state.first_seat);
		wl_display_flush(state.display);
		dispatch_for(state.display, 500);
	}

	int count = 0;
	int with_output = 0;
	struct Top *top;
	wl_list_for_each(top, &state.tops, link) {
		if (top->closed) continue;
		count++;
		if (top->output_enters > 0) with_output++;
		printf("top title=\"%s\" app_id=\"%s\" activated=%d output_enters=%d done=%d\n",
			top->title ? top->title : "",
			top->app_id ? top->app_id : "",
			top->activated,
			top->output_enters,
			top->done_count);
	}

	printf("summary outputs=%d first_output_name=%u seats=%d first_seat_name=%u foreign_globals=%d toplevels=%d with_output=%d\n",
		state.outputs, state.first_output_name, state.seats, state.first_seat_name,
		state.foreign_globals, count, with_output);
	return count > 0 ? 0 : 3;
}
