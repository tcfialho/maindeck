#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <ctype.h>

#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <river-input-management-v1-client-protocol.h>
#include <cursor-shape-v1-client-protocol.h>

#include "types.h"
#include "wm-log.h"
#include "wm-state.h"
#include "wm-layout.h"
#include "wm-input.h"
#include "wm-libinput.h"
#include "wm-handlers.h"
#include "wm-config.h"

static void wm_notify_bar_fullscreen(bool on) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (dir == NULL || dir[0] == '\0') return;
	char path[108];
	if ((size_t)snprintf(path, sizeof(path), "%s/maindeck-bar.sock", dir) >= sizeof(path)) return;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	const char *msg = on ? "fullscreen_on" : "fullscreen_off";
	sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&addr, sizeof(addr));
	close(fd);
}

static void clear_loading_notification(void) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (dir == NULL || dir[0] == '\0') dir = "/tmp";

	char path[512];
	if ((size_t)snprintf(path, sizeof(path), "%s/maindeck-loading.id", dir) >= sizeof(path)) {
		return;
	}

	FILE *f = fopen(path, "r");
	if (f == NULL) return;

	char id[64];
	if (fgets(id, sizeof(id), f) == NULL) id[0] = '\0';
	fclose(f);
	unlink(path);

	char *nl = strchr(id, '\n');
	if (nl != NULL) *nl = '\0';
	if (id[0] == '\0') return;

	if (fork() == 0) {
		execlp("makoctl", "makoctl", "dismiss", "-n", id, (char *)0);
		_exit(127);
	}
}

static void output_handle_removed(void *data, struct river_output_v1 *obj) {
	struct Output *output = data;
	output->removed = true;
}

static void output_handle_position(void *data, struct river_output_v1 *obj, int32_t x, int32_t y) {
	struct Output *output = data;
	output->x = x;
	output->y = y;
}

static void output_handle_dimensions(void *data, struct river_output_v1 *obj, int32_t width, int32_t height) {
	struct Output *output = data;
	output->width = width;
	output->height = height;
}

static void output_handle_wl_output(void *data, struct river_output_v1 *obj, uint32_t name) {}

static void output_handle_capture_sessions(void *data, struct river_output_v1 *obj, uint32_t count) {
	struct Output *output = data;
	(void)obj;
	if (output->capture_session_count != count) {
		LOG_EVENT("output capture sessions: %u", count);
	}
	output->capture_session_count = count;
	if (count == 0) {
		output->scanout_capture_logged = false;
	}
}

static const struct river_output_v1_listener river_output_listener = {
	.removed = output_handle_removed,
	.wl_output = output_handle_wl_output,
	.position = output_handle_position,
	.dimensions = output_handle_dimensions,
	.capture_sessions = output_handle_capture_sessions,
};

static void layer_shell_output_handle_non_exclusive_area(
		void *data, struct river_layer_shell_output_v1 *obj,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	struct Output *output = data;
	output->usable_x = x;
	output->usable_y = y;
	output->usable_width = width;
	output->usable_height = height;
	LOG_INFO("non_exclusive_area: x=%d y=%d w=%d h=%d (output %dx%d)",
		x, y, width, height, output->width, output->height);
}

static const struct river_layer_shell_output_v1_listener layer_shell_output_listener = {
	.non_exclusive_area = layer_shell_output_handle_non_exclusive_area,
};

void output_maybe_destroy(struct Output *output) {
	if (!output->removed) return;
	if (output->shell_output != NULL) {
		river_layer_shell_output_v1_destroy(output->shell_output);
		output->shell_output = NULL;
	}
	river_output_v1_destroy(output->obj);
	wl_list_remove(&output->link);
	free(output);
}

static void window_destroy_closed(struct Window *window, bool flush_now);

static void window_handle_closed(void *data, struct river_window_v1 *obj) {
	struct Window *window = data;
	if (md_verbose()) {
		LOG_EVENT("window closed: \"%s\" app_id=%s index=%d",
			window->title ? window->title : "",
			window->app_id ? window->app_id : "",
			window_index(window));
	}
	window->closed = true;
	if (window->parent != NULL) {
		window_destroy_closed(window, true);
	}
}

static void window_handle_dimensions(void *data, struct river_window_v1 *obj, int32_t width, int32_t height) {
	struct Window *window = data;
	if (window->width != width || window->height != height) {
		window->width = width;
		window->height = height;
		if (window->parent != NULL && !window->closed && window_is_really_visible(window)) {
			river_window_manager_v1_manage_dirty(window_manager_v1);
		}
	}
}

struct ImplicitParentRule {
	bool loaded;
	bool enabled;
	char app_id[128];
	char parent_titles[512]; // '|' separated exact titles
};

static struct ImplicitParentRule implicit_parent_rule(void) {
	static struct ImplicitParentRule rule;
	if (!rule.loaded) {
		const char *app_id = getenv("MAINDECK_IMPLICIT_PARENT_APP_ID");
		const char *titles = getenv("MAINDECK_IMPLICIT_PARENT_TITLES");
		if (app_id != NULL && app_id[0] != '\0' && titles != NULL && titles[0] != '\0') {
			snprintf(rule.app_id, sizeof(rule.app_id), "%s", app_id);
			snprintf(rule.parent_titles, sizeof(rule.parent_titles), "%s", titles);
			rule.enabled = true;
		}
		rule.loaded = true;
	}
	return rule;
}

static bool implicit_parent_app_matches(const char *app_id) {
	struct ImplicitParentRule rule = implicit_parent_rule();
	return rule.enabled && app_id != NULL && strcasecmp(app_id, rule.app_id) == 0;
}

static bool implicit_parent_title_matches(const char *title) {
	if (title == NULL) return false;
	struct ImplicitParentRule rule = implicit_parent_rule();
	if (!rule.enabled) return false;

	const char *start = rule.parent_titles;
	while (*start != '\0') {
		const char *end = strchr(start, '|');
		size_t len = end ? (size_t)(end - start) : strlen(start);
		if (strlen(title) == len && strncmp(title, start, len) == 0) {
			return true;
		}
		if (end == NULL) break;
		start = end + 1;
	}
	return false;
}

static bool window_should_float(const char *app_id) {
	return wm_config_should_float(app_id);
}

static void maybe_apply_implicit_parenting(void) {
	struct ImplicitParentRule rule = implicit_parent_rule();
	if (!rule.enabled) return;

	struct Window *implicit_parent = NULL;
	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		if (w->parent == NULL && !w->floating &&
		    implicit_parent_app_matches(w->app_id) &&
		    implicit_parent_title_matches(w->title)) {
			implicit_parent = w;
			break;
		}
	}
	if (implicit_parent != NULL) {
		struct Window *child, *child_tmp;
		bool changed = false;
		wl_list_for_each_safe(child, child_tmp, &wm.windows, link) {
			if (child != implicit_parent &&
			    child->parent == NULL && !child->floating &&
			    implicit_parent_app_matches(child->app_id) &&
			    child->title != NULL &&
			    !implicit_parent_title_matches(child->title)) {
				child->parent = implicit_parent;
				child->implicit_parent = true;
				child->transient_size_proposed = false;
				move_last(child);
				changed = true;
				LOG_EVENT("window became child (implicit): \"%s\" parent=\"%s\" app_id=%s",
				          child->title ? child->title : "",
				          implicit_parent->title ? implicit_parent->title : "",
				          child->app_id ? child->app_id : "");
			}
		}
		if (changed) {
			river_window_manager_v1_manage_dirty(window_manager_v1);
		}
	}
}

static void maybe_apply_implicit_parenting_for(struct Window *window, bool title_signal_is_relevant) {
	if (window->closed || window->parent != NULL || window->floating || !implicit_parent_app_matches(window->app_id)) return;
	if (!title_signal_is_relevant && !implicit_parent_title_matches(window->title)) return;
	maybe_apply_implicit_parenting();
}

static void window_handle_app_id(void *data, struct river_window_v1 *obj, const char *app_id) {
	struct Window *window = data;
	window_set_string(&window->app_id, app_id);

	if (window_should_float(app_id) && !window->floating) {
		window->floating = true;
		move_last(window);
		LOG_EVENT("window designated as floating: app_id=%s title=\"%s\"",
		          app_id ? app_id : "", window->title ? window->title : "");

		// Focus it immediately on all seats!
		struct Seat *seat;
		wl_list_for_each(seat, &wm.seats, link) {
			if (seat->removed) continue;
			river_seat_v1_clear_focus(seat->obj);
			river_seat_v1_focus_window(seat->obj, window->obj);
			seat->focused = window;
		}

		river_window_manager_v1_manage_dirty(window_manager_v1);
	}

	maybe_apply_implicit_parenting_for(window, true);
}

static void window_handle_title(void *data, struct river_window_v1 *obj, const char *title) {
	struct Window *window = data;
	bool had_title = window->title != NULL && window->title[0] != '\0';
	window_set_string(&window->title, title);
	bool title_signal_is_relevant = !had_title || implicit_parent_title_matches(window->title);
	maybe_apply_implicit_parenting_for(window, title_signal_is_relevant);
}

static void window_handle_dimensions_hint(void *data, struct river_window_v1 *obj, int32_t min_width, int32_t min_height, int32_t max_width, int32_t max_height) {
	struct Window *window = data;
	window->min_width = min_width;
	window->min_height = min_height;
	window->max_width = max_width;
	window->max_height = max_height;
}
static void window_handle_parent(void *data, struct river_window_v1 *obj, struct river_window_v1 *parent) {
	struct Window *window = data;
	(void)obj;
	struct Window *new_parent = parent ? river_window_v1_get_user_data(parent) : NULL;
	if (new_parent == window) new_parent = NULL; // Prevent self-parenting
	if (window->parent == new_parent) {
		if (new_parent != NULL) window->implicit_parent = false;
		return;
	}

	bool was_child = (window->parent != NULL);
	bool becomes_child = (new_parent != NULL);

	window->parent = new_parent;
	window->implicit_parent = false;
	window->transient_size_proposed = false;

	if (!was_child && becomes_child) {
		// Was a root window occupying a slot in the list; move to tail so it
		// no longer shifts other root windows' indices.
		move_last(window);
		LOG_EVENT("window became child: \"%s\" parent=\"%s\"",
		          window->title ? window->title : "",
		          new_parent->title ? new_parent->title : "");
	} else if (was_child && !becomes_child) {
		// Was a child, now becomes an independent root window: insert as new MAIN.
		wl_list_remove(&window->link);
		md_insert_new_window(window);
		LOG_EVENT("window became root (parent cleared): \"%s\"",
		          window->title ? window->title : "");
	}

	river_window_manager_v1_manage_dirty(window_manager_v1);
}
static void window_handle_decoration_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {}
static void window_handle_pointer_move_requested(void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat) {}
static void window_handle_pointer_resize_requested(void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat, uint32_t edges) {}
static void window_handle_show_window_menu_requested(void *data, struct river_window_v1 *obj, int32_t x, int32_t y) {}
static void window_handle_maximize_requested(void *data, struct river_window_v1 *obj) {}
static void window_handle_unmaximize_requested(void *data, struct river_window_v1 *obj) {}
static void window_handle_fullscreen_requested(void *data, struct river_window_v1 *obj, struct river_output_v1 *river_output) {
	struct Window *window = data;
	window->fullscreen = true;
	window->fs_output = river_output;
	if (window->minimized) return; // fullscreen honrado no restore; não move na lista
	int32_t idx = window_index(window);
	if (idx == 0 || idx == 1) {
		wm.target_index = idx;
	} else if (idx >= 2) {
		move_first(window);
		wm.target_index = 0;
	}
	wm.maximized = false;
	LOG_EVENT("fullscreen requested: \"%s\" app_id=%s", window->title ? window->title : "", window->app_id ? window->app_id : "");
	wm_notify_bar_fullscreen(true);
}
static void window_handle_exit_fullscreen_requested(void *data, struct river_window_v1 *obj) {
	struct Window *window = data;
	window->fullscreen = false;
	window->fs_output = NULL;
	LOG_EVENT("exit fullscreen requested: \"%s\" app_id=%s", window->title ? window->title : "", window->app_id ? window->app_id : "");
	wm_notify_bar_fullscreen(false);
}
static void window_handle_minimize_requested(void *data, struct river_window_v1 *obj) {
	(void)obj;
	struct Window *window = data;
	if (window->closed) return;
	if (window->minimized) return;
	if (window->fullscreen) {
		window->fullscreen = false;
		window->fs_output = NULL;
		wm_notify_bar_fullscreen(false);
	}
	window->minimized = true;
	move_last(window);
	wm.target_index = 0;
	wm.maximized = false;
	LOG_EVENT("minimize requested: \"%s\" app_id=%s",
		window->title ? window->title : "",
		window->app_id ? window->app_id : "");
}
static void window_handle_unreliable_pid(void *data, struct river_window_v1 *obj, int32_t unreliable_pid) {}
static void window_handle_presentation_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {
	struct Window *window = data;
	(void)obj;
	if (hint != RIVER_OUTPUT_V1_PRESENTATION_MODE_VSYNC &&
	    hint != RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC) {
		LOG_WARN("invalid presentation hint ignored: %u", hint);
		return;
	}
	window->presentation_hint = hint;
}
static void window_handle_identifier(void *data, struct river_window_v1 *obj, const char *identifier) {
	struct Window *window = data;
	(void)obj;
	if (!valid_identifier(identifier)) {
		LOG_WARN("window_handle_identifier: identifier inválido ou nulo, ignorando");
		return;
	}
	window_set_string(&window->identifier, identifier);
	LOG_EVENT("window identifier: title=\"%s\" app_id=\"%s\" id=%s",
	          window->title ? window->title : "",
	          window->app_id ? window->app_id : "",
	          identifier);
}

static void window_handle_capture_sessions(void *data, struct river_window_v1 *obj, uint32_t count) {
	struct Window *window = data;
	(void)obj;
	if (window->capture_session_count != count) {
		LOG_EVENT("window capture sessions: %u title=\"%s\" app_id=%s",
		          count,
		          window->title ? window->title : "",
		          window->app_id ? window->app_id : "");
	}
	window->capture_session_count = count;
}

static const struct river_window_v1_listener river_window_listener = {
	.closed = window_handle_closed,
	.dimensions_hint = window_handle_dimensions_hint,
	.dimensions = window_handle_dimensions,
	.app_id = window_handle_app_id,
	.title = window_handle_title,
	.parent = window_handle_parent,
	.decoration_hint = window_handle_decoration_hint,
	.pointer_move_requested = window_handle_pointer_move_requested,
	.pointer_resize_requested = window_handle_pointer_resize_requested,
	.show_window_menu_requested = window_handle_show_window_menu_requested,
	.maximize_requested = window_handle_maximize_requested,
	.unmaximize_requested = window_handle_unmaximize_requested,
	.fullscreen_requested = window_handle_fullscreen_requested,
	.exit_fullscreen_requested = window_handle_exit_fullscreen_requested,
	.minimize_requested = window_handle_minimize_requested,
	.unreliable_pid = window_handle_unreliable_pid,
	.presentation_hint = window_handle_presentation_hint,
	.identifier = window_handle_identifier,
	.capture_sessions = window_handle_capture_sessions,
};

static void window_destroy_closed(struct Window *window, bool flush_now) {
	if (window->fullscreen)
		wm_notify_bar_fullscreen(false);
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->focused == window) seat->focused = NULL;
		if (seat->interacted == window) seat->interacted = NULL;
	}
	if (wm.last_placed_top_node == window->node) {
		wm.last_placed_top_node = NULL;
	}
	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		if (w->parent == window) {
			w->parent = NULL;
			w->implicit_parent = false;
			w->transient_size_proposed = false;
		}
	}
	river_window_v1_destroy(window->obj);
	if (flush_now && wm_display != NULL) {
		int rc = wl_display_flush(wm_display);
		if (rc < 0) {
			if (errno != EAGAIN) {
				LOG_WARN("flush after child destroy failed: %s", strerror(errno));
			}
		}
	}
	wl_list_remove(&window->link);
	free(window->app_id);
	free(window->title);
	free(window->identifier);
	free(window);
}

void window_maybe_destroy(struct Window *window) {
	if (!window->closed) return;
	window_destroy_closed(window, false);
}

static void wm_handle_unavailable(void *data, struct river_window_manager_v1 *obj) {
	LOG_WARN("another window manager is already running");
	exit(1);
}

static void wm_handle_finished(void *data, struct river_window_manager_v1 *obj) {
	LOG_INFO("session finished");
	exit(0);
}

static uint64_t g_layout_sig = 0;
static bool g_layout_sig_fresh = false;

static uint64_t compute_layout_signature(const struct LayoutView *view) {
	uint64_t h = 1469598103934665603ULL;
	#define SIG_MIX(val) do { h = (h ^ (uint64_t)(val)) * 1099511628211ULL; } while (0)

	SIG_MIX(view->target_index);
	SIG_MIX(view->maximized);
	SIG_MIX(view->visible_count);
	SIG_MIX((uintptr_t)view->target);
	SIG_MIX((uintptr_t)view->main_win);
	SIG_MIX((uintptr_t)view->deck_win);
	SIG_MIX(view->single);
	SIG_MIX(view->output.x);
	SIG_MIX(view->output.y);
	SIG_MIX(view->output.width);
	SIG_MIX(view->output.height);

	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		SIG_MIX((uintptr_t)w->obj);
		SIG_MIX(w->fullscreen);
		SIG_MIX(w->minimized);
		SIG_MIX(w->floating);
		SIG_MIX((uintptr_t)w->parent);
		if (w->parent != NULL || w->floating) {
			SIG_MIX((uint32_t)w->width);
			SIG_MIX((uint32_t)w->height);
		}
	}

	struct Output *o;
	wl_list_for_each(o, &wm.outputs, link) {
		SIG_MIX(o->removed);
		SIG_MIX((uint32_t)o->x);
		SIG_MIX((uint32_t)o->y);
		SIG_MIX((uint32_t)o->width);
		SIG_MIX((uint32_t)o->height);
		SIG_MIX((uint32_t)o->usable_x);
		SIG_MIX((uint32_t)o->usable_y);
		SIG_MIX((uint32_t)o->usable_width);
		SIG_MIX((uint32_t)o->usable_height);
	}

	struct Seat *s;
	wl_list_for_each(s, &wm.seats, link) {
		if (s->removed) continue;
		SIG_MIX((uintptr_t)s->obj);
	}

	#undef SIG_MIX
	return h;
}

static struct river_output_v1 *window_fullscreen_output(struct Window *window) {
	if (window->fs_output != NULL) return window->fs_output;
	struct Output *output = active_output();
	return output ? output->obj : NULL;
}

static bool window_controls_output_presentation(struct Window *window, struct Output *output) {
	return window != NULL &&
	       output != NULL &&
	       output->obj != NULL &&
	       window->fullscreen &&
	       !window->minimized &&
	       window->applied_fullscreen &&
	       window_fullscreen_output(window) == output->obj;
}

static struct Window *fullscreen_window_for_output(struct Output *output) {
	struct Window *target = target_window();
	if (window_controls_output_presentation(target, output)) {
		return target;
	}

	struct Window *candidate = NULL;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window_controls_output_presentation(window, output)) {
			candidate = window;
		}
	}
	return candidate;
}

static bool window_is_likely_game(struct Window *window) {
	if (window == NULL) return false;
	if (window->app_id == NULL) return false;

	// Heuristics:
	// 1. Steam games: app_id starts with "steam_app_" (e.g. steam_app_12345)
	if (strncmp(window->app_id, "steam_app_", 10) == 0) return true;

	// 2. Proton/Wine games: app_id ends with ".exe" (e.g. cyberpunk2077.exe)
	size_t len = strlen(window->app_id);
	if (len > 4 && strcasecmp(window->app_id + len - 4, ".exe") == 0) return true;

	// 3. Proton/Wine/Lutris prefixes/names: contains "wine", "proton", "retroarch"
	if (strcasestr(window->app_id, "wine") != NULL) return true;
	if (strcasestr(window->app_id, "proton") != NULL) return true;
	if (strcasestr(window->app_id, "retroarch") != NULL) return true;
	if (strcasestr(window->app_id, "heroic") != NULL) return true;
	if (strcasestr(window->app_id, "lutris") != NULL) return true;

	return false;
}

static void apply_output_presentation_modes(void) {
	struct Output *output;
	wl_list_for_each(output, &wm.outputs, link) {
		if (output->removed || output->obj == NULL) continue;

		struct Window *window = fullscreen_window_for_output(output);
		if (window == NULL) {
			output->scanout_capture_logged = false;
		}
		uint32_t mode = RIVER_OUTPUT_V1_PRESENTATION_MODE_VSYNC;
		if (window != NULL &&
		    (window->presentation_hint == RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC ||
		     g_wm_config.force_tearing_fullscreen ||
		     window_is_likely_game(window))) {
			mode = RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC;
		}
		if (window != NULL && output->capture_session_count > 0 && !output->scanout_capture_logged) {
			LOG_EVENT("direct scanout blocked: output capture_sessions=%u window=\"%s\" app_id=%s",
				output->capture_session_count,
				window->title ? window->title : "",
				window->app_id ? window->app_id : "");
			output->scanout_capture_logged = true;
		}

		if (output->presentation_mode_set && output->presentation_mode == mode) continue;

		river_output_v1_set_presentation_mode(output->obj, mode);
		if (mode == RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC || output->presentation_mode_set) {
			LOG_EVENT("presentation mode: %s window=\"%s\" app_id=%s",
				mode == RIVER_OUTPUT_V1_PRESENTATION_MODE_ASYNC ? "async" : "vsync",
				window && window->title ? window->title : "",
				window && window->app_id ? window->app_id : "");
		}
		output->presentation_mode = mode;
		output->presentation_mode_set = true;
	}
}

static void wm_handle_manage_start(void *data, struct river_window_manager_v1 *obj) {
	struct Output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &wm.outputs, link) {
		output_maybe_destroy(output);
	}
	struct Window *window, *window_tmp;
	wl_list_for_each_safe(window, window_tmp, &wm.windows, link) {
		window_maybe_destroy(window);
	}
	struct Seat *seat, *seat_tmp;
	wl_list_for_each_safe(seat, seat_tmp, &wm.seats, link) {
		seat_maybe_destroy(seat);
	}

	process_hold_timers();

	wl_list_for_each(seat, &wm.seats, link) {
		seat_manage(seat);
	}
	apply_pending_taskbar_activation();
	clamp_target();

	struct Output *primary = NULL;
	wl_list_for_each(output, &wm.outputs, link) {
		if (!output->removed && output->shell_output != NULL) {
			primary = output;
			break;
		}
	}
	if (primary != NULL && !primary->default_set) {
		river_layer_shell_output_v1_set_default(primary->shell_output);
		primary->default_set = true;
	}

	struct LayoutView view;
	layout_view_init(&view);

	static uint64_t last_layout_sig = 0;
	static bool have_last_sig = false;
	uint64_t sig = compute_layout_signature(&view);
	g_layout_sig = sig;
	g_layout_sig_fresh = true;

	bool layout_changed = (!have_last_sig || sig != last_layout_sig);
	if (layout_changed) {
		size_t index = 0;
		wl_list_for_each(window, &wm.windows, link) {
			if (window->parent != NULL) {
				window_manage_layout(window, 0, &view);
				continue;
			}
			window_manage_layout(window, index, &view);
			if (!window->floating && !window->minimized) {
				index++;
			}
		}
		last_layout_sig = sig;
		have_last_sig = true;
	}

	if (layout_changed || wm.focus_dirty) {
		focus_target_on_seats();
		log_state();
		wm.focus_dirty = false;
	}

	river_window_manager_v1_manage_finish(window_manager_v1);
}

static void wm_handle_render_start(void *data, struct river_window_manager_v1 *obj) {
	static uint64_t last_render_sig = 0;
	static bool have_last_render_sig = false;
	uint64_t sig;
	struct LayoutView view;
	if (g_layout_sig_fresh) {
		sig = g_layout_sig;
		g_layout_sig_fresh = false;
		layout_view_init(&view);
	} else {
		layout_view_init(&view);
		sig = compute_layout_signature(&view);
	}

	apply_output_presentation_modes();

	if (!have_last_render_sig || sig != last_render_sig) {
		size_t index = 0;
		struct Window *window;
		wl_list_for_each(window, &wm.windows, link) {
			if (window->parent != NULL) continue;
			window_render_layout(window, index, &view);
			if (!window->floating && !window->minimized) {
				index++;
			}
		}
		if (view.target != NULL) {
			wm_place_top(view.target->node);
		}
		wl_list_for_each(window, &wm.windows, link) {
			if (window->parent == NULL) continue;
			window_render_layout(window, 0, &view);
		}
		last_render_sig = sig;
		have_last_render_sig = true;
	}
	river_window_manager_v1_render_finish(window_manager_v1);
}

static void wm_handle_window(void *data, struct river_window_manager_v1 *obj, struct river_window_v1 *river_window) {
	struct Window *window = calloc(1, sizeof(struct Window));
	if (window == NULL) {
		LOG_WARN("calloc(Window) failed; dropping window (OOM)");
		return;
	}
	window->obj = river_window;
	window->node = river_window_v1_get_node(window->obj);
	window->new = true;
	river_window_v1_add_listener(window->obj, &river_window_listener, window);
	md_insert_new_window(window);
	clear_loading_notification();
}

static void wm_handle_output(void *data, struct river_window_manager_v1 *obj, struct river_output_v1 *river_output) {
	struct Output *output = calloc(1, sizeof(struct Output));
	if (output == NULL) {
		LOG_WARN("calloc(Output) failed; dropping output (OOM)");
		return;
	}
	output->obj = river_output;
	river_output_v1_add_listener(output->obj, &river_output_listener, output);
	wl_list_insert(wm.outputs.prev, &output->link);

	if (layer_shell_v1 != NULL) {
		output->shell_output = river_layer_shell_v1_get_output(layer_shell_v1, river_output);
		river_layer_shell_output_v1_add_listener(output->shell_output, &layer_shell_output_listener, output);
	}
}

static void layer_shell_seat_handle_focus_exclusive(void *data, struct river_layer_shell_seat_v1 *obj) {
	LOG_EVENT("layer_shell_seat: focus_exclusive");
}

static void layer_shell_seat_handle_focus_non_exclusive(void *data, struct river_layer_shell_seat_v1 *obj) {
	LOG_EVENT("layer_shell_seat: focus_non_exclusive");
}

static void layer_shell_seat_handle_focus_none(void *data, struct river_layer_shell_seat_v1 *obj) {
	LOG_EVENT("layer_shell_seat: focus_none -> setting focus_dirty");
	wm.focus_dirty = true;
}

static const struct river_layer_shell_seat_v1_listener layer_shell_seat_listener = {
	.focus_exclusive = layer_shell_seat_handle_focus_exclusive,
	.focus_non_exclusive = layer_shell_seat_handle_focus_non_exclusive,
	.focus_none = layer_shell_seat_handle_focus_none,
};

static void wm_handle_seat(void *data, struct river_window_manager_v1 *obj, struct river_seat_v1 *river_seat) {
	struct Seat *seat = calloc(1, sizeof(struct Seat));
	if (seat == NULL) {
		LOG_WARN("calloc(Seat) failed; dropping seat (OOM)");
		return;
	}
	seat->obj = river_seat;
	seat->new = true;
	wl_list_init(&seat->xkb_bindings);
	wl_list_init(&seat->pointer_bindings);
	river_seat_v1_add_listener(seat->obj, &river_seat_listener, seat);
	wl_list_insert(wm.seats.prev, &seat->link);

	if (layer_shell_v1 != NULL) {
		seat->layer_shell_seat = river_layer_shell_v1_get_seat(layer_shell_v1, seat->obj);
		river_layer_shell_seat_v1_add_listener(seat->layer_shell_seat, &layer_shell_seat_listener, seat);
	}
}

static void wm_handle_session_locked(void *data, struct river_window_manager_v1 *obj) {}
static void wm_handle_session_unlocked(void *data, struct river_window_manager_v1 *obj) {}

const struct river_window_manager_v1_listener wm_listener = {
	.unavailable = wm_handle_unavailable,
	.finished = wm_handle_finished,
	.manage_start = wm_handle_manage_start,
	.render_start = wm_handle_render_start,
	.session_locked = wm_handle_session_locked,
	.session_unlocked = wm_handle_session_unlocked,
	.window = wm_handle_window,
	.output = wm_handle_output,
	.seat = wm_handle_seat,
};

void wm_init(void) {
	wl_list_init(&wm.outputs);
	wl_list_init(&wm.windows);
	wl_list_init(&wm.seats);
	wm.target_index = 0;
	wm.maximized = false;
}

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
		if (version >= 4) {
			window_manager_v1 = wl_registry_bind(registry, name, &river_window_manager_v1_interface,
				version >= 5 ? 5 : 4);
		}
	} else if (strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
		xkb_bindings_v1 = wl_registry_bind(registry, name, &river_xkb_bindings_v1_interface, 1);
	} else if (strcmp(interface, river_layer_shell_v1_interface.name) == 0) {
		layer_shell_v1 = wl_registry_bind(registry, name, &river_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, river_libinput_config_v1_interface.name) == 0) {
		libinput_config_v1 = wl_registry_bind(registry, name, &river_libinput_config_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		cursor_shape_manager_v1 = wl_registry_bind(registry, name,
			&wp_cursor_shape_manager_v1_interface, version < 1 ? version : 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};
