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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define BORDER_WIDTH 3
#define HOLD_THRESHOLD_MS 360

// --- Logging ---

static FILE *log_file;

static void log_init(void) {
	const char *home = getenv("HOME");
	if (home == NULL) home = "/tmp";
	char path[512];
	snprintf(path, sizeof(path), "%s/.local/state/maindeck", home);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.local/state/maindeck/maindeck.log", home);
	log_file = fopen(path, "a");
	if (log_file != NULL) {
		setvbuf(log_file, NULL, _IOLBF, 0);
	}
}

static void md_log(const char *level, const char *fmt, ...) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm *t = localtime(&ts.tv_sec);
	char tbuf[32];
	strftime(tbuf, sizeof(tbuf), "%H:%M:%S", t);

	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	fprintf(stderr, "[%s.%03ld] [%s] %s\n", tbuf, ts.tv_nsec / 1000000, level, msg);
	if (log_file != NULL) {
		fprintf(log_file, "[%s.%03ld] [%s] %s\n", tbuf, ts.tv_nsec / 1000000, level, msg);
	}
}

#define LOG_INFO(...)  md_log("INFO ", __VA_ARGS__)
#define LOG_WARN(...)  md_log("WARN ", __VA_ARGS__)
#define LOG_EVENT(...) md_log("EVENT", __VA_ARGS__)
#define LOG_STATE(...) md_log("STATE", __VA_ARGS__)

struct Output {
	struct river_output_v1 *obj;
	struct river_layer_shell_output_v1 *shell_output;
	bool removed;
	bool default_set;
	int32_t x, y, width, height;
	// Área utilizável após subtrair as exclusive zones das layer shells (waybar).
	// Zero até o primeiro non_exclusive_area chegar — fallback para dimensões do output.
	int32_t usable_x, usable_y, usable_width, usable_height;
	struct wl_list link;
};

struct Window {
	struct river_window_v1 *obj;
	struct river_node_v1 *node;

	bool new;
	bool closed;
	int32_t width, height;
	char *app_id;
	char *title;

	struct wl_list link; // WindowManager.windows in MainDeck order
};

enum Action {
	ACTION_NONE,
	ACTION_SPAWN_TERMINAL,
	ACTION_SPAWN_LAUNCHER,
	ACTION_CLOSE_TARGET,
	ACTION_TOGGLE_TARGET,
	ACTION_SWAP_MAIN_DECK,
	ACTION_DECK_NEXT,
	ACTION_DECK_PREV,
	ACTION_SEND_TARGET_TO_DECK_BOTTOM,
	ACTION_PROMOTE_TARGET_TO_MAIN,
	ACTION_MAXIMIZE_TARGET,
	ACTION_RESTORE,
	ACTION_EXIT,
};

struct XkbBinding {
	struct river_xkb_binding_v1 *obj;
	struct Seat *seat;
	enum Action tap_action;
	enum Action hold_action; // ACTION_NONE if no hold behavior
	bool held;
	bool hold_fired;
	struct timespec press_time;
	struct wl_list link;
};

struct PointerBinding {
	struct river_pointer_binding_v1 *obj;
	struct Seat *seat;
	enum Action action;
	struct wl_list link;
};

struct Seat {
	struct river_seat_v1 *obj;
	bool new;
	bool removed;

	struct Window *focused;
	struct Window *hovered;
	struct Window *interacted;

	struct wl_list xkb_bindings;
	struct wl_list pointer_bindings;
	enum Action pending_action;

	struct wl_list link;
};

struct WindowManager {
	struct wl_list outputs;
	struct wl_list windows; // MainDeck order: 0=MAIN, 1=visible DECK, 2+=hidden
	struct wl_list seats;
	uint32_t target_index; // 0 or 1
	bool maximized;
};

static struct WindowManager wm;
static struct river_window_manager_v1 *window_manager_v1;
static struct river_xkb_bindings_v1 *xkb_bindings_v1;
static struct river_layer_shell_v1 *layer_shell_v1;

static uint32_t chan(uint8_t value) {
	return (uint32_t)value * 0x01010101u;
}

static uint32_t all_edges(void) {
	return RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;
}

static size_t window_count(void) {
	size_t count = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		count++;
	}
	return count;
}

static struct Window *window_at(size_t index) {
	size_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (i == index) return window;
		i++;
	}
	return NULL;
}

static int32_t window_index(struct Window *needle) {
	int32_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window == needle) return i;
		i++;
	}
	return -1;
}

static struct Window *target_window(void) {
	return window_at(wm.target_index);
}

static void clamp_target(void) {
	size_t count = window_count();
	if (count == 0) {
		wm.target_index = 0;
		wm.maximized = false;
		return;
	}
	if (wm.target_index > 1 || wm.target_index >= count) {
		wm.target_index = 0;
	}
}

static struct Output *active_output(void) {
	struct Output *fallback = NULL;
	struct Output *output;
	wl_list_for_each(output, &wm.outputs, link) {
		if (output->removed) continue;
		if (fallback == NULL) fallback = output;
		if (output->width > 0 && output->height > 0) return output;
	}
	return fallback;
}

struct Box {
	int32_t x, y, width, height;
};

static struct Box output_box(void) {
	struct Output *output = active_output();
	if (output == NULL || output->width <= 0 || output->height <= 0) {
		return (struct Box){ .x = 0, .y = 0, .width = DEFAULT_WIDTH, .height = DEFAULT_HEIGHT };
	}
	if (output->usable_width > 0 && output->usable_height > 0) {
		return (struct Box){
			.x = output->usable_x, .y = output->usable_y,
			.width = output->usable_width, .height = output->usable_height,
		};
	}
	return (struct Box){ .x = output->x, .y = output->y, .width = output->width, .height = output->height };
}

static struct Box layout_box_for_index(size_t index) {
	struct Box out = output_box();
	size_t count = window_count();
	if (count <= 1 || wm.maximized) {
		return out;
	}

	int32_t main_width = (out.width * 2) / 3;
	if (main_width < 1) main_width = out.width;
	int32_t deck_width = out.width - main_width;
	if (deck_width < 1) deck_width = 1;

	if (index == 0) {
		return (struct Box){ .x = out.x, .y = out.y, .width = main_width, .height = out.height };
	}
	return (struct Box){ .x = out.x + main_width, .y = out.y, .width = deck_width, .height = out.height };
}

static bool window_is_visible_index(size_t index) {
	if (wm.maximized) return index == wm.target_index;
	return index < 2;
}

static void move_after(struct Window *window, struct wl_list *position) {
	wl_list_remove(&window->link);
	wl_list_insert(position, &window->link);
}

static void move_first(struct Window *window) {
	move_after(window, &wm.windows);
}

static void move_last(struct Window *window) {
	move_after(window, wm.windows.prev);
}

static void spawn_command(const char *cmd) {
	if (fork() == 0) {
		execlp(cmd, cmd, (char *)0);
		_exit(127);
	}
}

static void spawn_sh(const char *cmd) {
	if (fork() == 0) {
		execlp("sh", "sh", "-c", cmd, (char *)0);
		_exit(127);
	}
}

// When the user clicks/focuses a normal window while the fuzzel launcher is
// open, it should dismiss (Windows-like). fuzzel is a layer-surface, so the
// compositor never reports it as a "window" — instead River sends us
// window_interaction (a normal window got focus) vs shell_surface_interaction
// (the layer surface itself was clicked). We close fuzzel on the former only.
//
// We deliberately do NOT track open-state in the WM: the launcher can be opened
// by the waybar click too, which never routes through us. So close_launcher is
// self-sufficient — it just pkills fuzzel (a no-op if none is running). The
// time guard suppresses the focus shuffle that the *open itself* causes (River
// reports window_interaction for the window that lost focus as fuzzel maps);
// without it the launcher would dismiss the instant it appears. last_launcher
// is stamped whenever WE spawn it; opens via waybar can't race us anyway because
// the click that opens lands on the bar (a layer surface), not a window.
static struct timespec last_launcher_spawn;
static bool last_launcher_spawn_valid = false;

static void close_launcher(void) {
	if (last_launcher_spawn_valid) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long since_ms = (now.tv_sec - last_launcher_spawn.tv_sec) * 1000L
			+ (now.tv_nsec - last_launcher_spawn.tv_nsec) / 1000000L;
		if (since_ms < 250) return; // ignore the open's own focus settling
	}
	if (fork() == 0) {
		execlp("pkill", "pkill", "-x", "fuzzel", (char *)0);
		_exit(127);
	}
}

static void window_set_string(char **field, const char *value) {
	free(*field);
	*field = value == NULL ? NULL : strdup(value);
}

static const char *window_label(struct Window *window) {
	if (window == NULL) return "(none)";
	if (window->title != NULL && window->title[0] != '\0') return window->title;
	if (window->app_id != NULL && window->app_id[0] != '\0') return window->app_id;
	return "(window)";
}

static void osd(const char *message) {
	LOG_INFO("OSD: %s", message);
	if (fork() == 0) {
		execlp("notify-send", "notify-send",
			"--expire-time=1200",
			"--urgency=low",
			"--app-name=maindeck",
			message, (char *)0);
		_exit(127);
	}
}

static void osd2(const char *a, const char *b) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s%s", a, b);
	osd(buf);
}

static void focus_target_on_seats(void) {
	clamp_target();
	struct Window *target = target_window();
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->removed) continue;
		if (target != NULL) {
			river_seat_v1_focus_window(seat->obj, target->obj);
		} else {
			river_seat_v1_clear_focus(seat->obj);
		}
		seat->focused = target;
	}
}

static void log_state(void) {
	size_t count = window_count();
	LOG_STATE("windows=%zu target=%u maximized=%d", count, wm.target_index, wm.maximized);
	size_t i = 0;
	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		const char *role = i == 0 ? "MAIN" : (i == 1 ? "DECK" : "hidden");
		const char *target = (i == wm.target_index) ? " [ALVO]" : "";
		LOG_STATE("  [%zu] %s \"%s\" app_id=%s%s", i, role,
			w->title ? w->title : "", w->app_id ? w->app_id : "", target);
		i++;
	}
}

static void md_swap_main_deck(void) {
	struct Window *main = window_at(0);
	struct Window *deck = window_at(1);
	if (main == NULL || deck == NULL) return;
	move_first(deck);
	wm.target_index = wm.target_index == 0 ? 1 : 0;
	wm.maximized = false;
	osd("MAIN \xe2\x86\x94 DECK"); // ↔
	log_state();
}

static void md_deck_next(void) {
	if (window_count() <= 2) {
		osd("sem janela invis\xc3\xadvel \xc3\xa0 direita");
		return;
	}
	struct Window *deck = window_at(1);
	move_last(deck);
	wm.target_index = 1;
	wm.maximized = false;
	osd2(window_label(window_at(1)), " entrou no DECK \xc2\xb7 ALVO");
	log_state();
}

static void md_deck_prev(void) {
	size_t count = window_count();
	if (count <= 2) {
		osd("sem janela invis\xc3\xadvel \xc3\xa0 esquerda");
		return;
	}
	struct Window *last = window_at(count - 1);
	struct Window *main = window_at(0);
	move_after(last, &main->link);
	wm.target_index = 1;
	wm.maximized = false;
	osd2(window_label(window_at(1)), " entrou no DECK \xc2\xb7 ALVO");
	log_state();
}

static void md_send_target_to_deck_bottom(void) {
	size_t count = window_count();
	if (count <= 1) return;
	struct Window *target = target_window();
	if (target == NULL) return;
	if (wm.target_index == 1 && count <= 2) return;
	const char *label = window_label(target);

	if (wm.target_index == 0) {
		// MAIN → move to DECK visible (index 1), ALVO follows the window there.
		// Old DECK slides up to MAIN; no hidden windows are affected.
		struct Window *deck = window_at(1);
		wl_list_remove(&target->link);
		wl_list_insert(&deck->link, &target->link); // deck=0, target=1
		wm.target_index = 1;
		osd2(label, " MAIN \xe2\x86\x92 DECK \xc2\xb7 ALVO");
	} else {
		// DECK → send to hidden bottom as before; next hidden window surfaces to DECK.
		move_last(target);
		clamp_target();
		osd2(label, " ao fundo do DECK");
	}

	wm.maximized = false;
	log_state();
}

static void md_promote_target_to_main(void) {
	struct Window *target = target_window();
	if (target == NULL || wm.target_index == 0) return;
	const char *label = window_label(target);
	move_first(target);
	wm.target_index = 0;
	wm.maximized = false;
	osd2(label, " \xe2\x86\x92 MAIN");
	log_state();
}

static void md_insert_new_window(struct Window *window) {
	size_t count = window_count();
	LOG_EVENT("new window: count_before=%zu", count);
	if (count == 0) {
		wl_list_insert(wm.windows.prev, &window->link);
	} else if (count == 1) {
		wl_list_insert(&wm.windows, &window->link);
	} else {
		struct Window *old_main = window_at(0);
		struct Window *deck = window_at(1);
		wl_list_insert(&wm.windows, &window->link);
		/* Defensive: window_at should be non-NULL when count>=2, but never
		 * deref a NULL on an unexpected list state — just insert at front. */
		if (old_main != NULL && deck != NULL) {
			move_after(old_main, &deck->link);
		}
		osd("nova janela em MAIN \xc2\xb7 DECK vis\xc3\xadvel preservado");
	}
	wm.target_index = 0;
	wm.maximized = false;
	log_state();
}

// --- Output handlers ---

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

static const struct river_output_v1_listener river_output_listener = {
	.removed = output_handle_removed,
	.wl_output = output_handle_wl_output,
	.position = output_handle_position,
	.dimensions = output_handle_dimensions,
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

static void output_maybe_destroy(struct Output *output) {
	if (!output->removed) return;
	if (output->shell_output != NULL) {
		river_layer_shell_output_v1_destroy(output->shell_output);
		output->shell_output = NULL;
	}
	river_output_v1_destroy(output->obj);
	wl_list_remove(&output->link);
	free(output);
}

// --- Window handlers ---

static void window_handle_closed(void *data, struct river_window_v1 *obj) {
	struct Window *window = data;
	LOG_EVENT("window closed: \"%s\" app_id=%s index=%d",
		window->title ? window->title : "",
		window->app_id ? window->app_id : "",
		window_index(window));
	window->closed = true;
}

static void window_handle_dimensions(void *data, struct river_window_v1 *obj, int32_t width, int32_t height) {
	struct Window *window = data;
	window->width = width;
	window->height = height;
}

static void window_handle_app_id(void *data, struct river_window_v1 *obj, const char *app_id) {
	struct Window *window = data;
	window_set_string(&window->app_id, app_id);
}

static void window_handle_title(void *data, struct river_window_v1 *obj, const char *title) {
	struct Window *window = data;
	window_set_string(&window->title, title);
}

static void window_handle_dimensions_hint(void *data, struct river_window_v1 *obj, int32_t min_width, int32_t min_height, int32_t max_width, int32_t max_height) {}
static void window_handle_parent(void *data, struct river_window_v1 *obj, struct river_window_v1 *parent) {}
static void window_handle_decoration_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {}
static void window_handle_pointer_move_requested(void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat) {}
static void window_handle_pointer_resize_requested(void *data, struct river_window_v1 *obj, struct river_seat_v1 *river_seat, uint32_t edges) {}
static void window_handle_show_window_menu_requested(void *data, struct river_window_v1 *obj, int32_t x, int32_t y) {}
static void window_handle_maximize_requested(void *data, struct river_window_v1 *obj) {}
static void window_handle_unmaximize_requested(void *data, struct river_window_v1 *obj) {}
static void window_handle_fullscreen_requested(void *data, struct river_window_v1 *obj, struct river_output_v1 *river_output) {}
static void window_handle_exit_fullscreen_requested(void *data, struct river_window_v1 *obj) {}
static void window_handle_minimize_requested(void *data, struct river_window_v1 *obj) {}
static void window_handle_unreliable_pid(void *data, struct river_window_v1 *obj, int32_t unreliable_pid) {}
static void window_handle_presentation_hint(void *data, struct river_window_v1 *obj, uint32_t hint) {}
static void window_handle_identifier(void *data, struct river_window_v1 *obj, const char *identifier) {}

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
};

static void window_maybe_destroy(struct Window *window) {
	if (!window->closed) return;
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->focused == window) seat->focused = NULL;
	}
	river_window_v1_destroy(window->obj);
	wl_list_remove(&window->link);
	free(window->app_id);
	free(window->title);
	free(window);
}

static void window_manage_layout(struct Window *window, size_t index) {
	if (!window_is_visible_index(index)) return;
	struct Box box = layout_box_for_index(index);
	int32_t width = box.width - (BORDER_WIDTH * 2);
	int32_t height = box.height - (BORDER_WIDTH * 2);
	river_window_v1_use_ssd(window->obj);
	river_window_v1_set_tiled(window->obj, all_edges());
	river_window_v1_propose_dimensions(window->obj, width > 1 ? width : 1, height > 1 ? height : 1);
	window->new = false;
}

static void window_render_layout(struct Window *window, size_t index) {
	if (!window_is_visible_index(index)) {
		river_window_v1_hide(window->obj);
		river_window_v1_set_borders(window->obj, RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
		return;
	}

	struct Box box = layout_box_for_index(index);
	river_window_v1_show(window->obj);
	river_node_v1_set_position(window->node, box.x + BORDER_WIDTH, box.y + BORDER_WIDTH);
	river_node_v1_place_top(window->node);

	if (index == wm.target_index) {
		// ALVO: yellow
		river_window_v1_set_borders(window->obj, all_edges(), BORDER_WIDTH,
			chan(245), chan(197), chan(66), 0xffffffffu);
	} else if (index == 0) {
		// MAIN: blue
		river_window_v1_set_borders(window->obj, all_edges(), BORDER_WIDTH,
			chan(59), chan(130), chan(246), 0xffffffffu);
	} else {
		// DECK: purple
		river_window_v1_set_borders(window->obj, all_edges(), BORDER_WIDTH,
			chan(139), chan(92), chan(246), 0xffffffffu);
	}
}

// --- XKB binding handlers (tap/hold) ---

static void xkb_binding_handle_pressed(void *data, struct river_xkb_binding_v1 *obj) {
	struct XkbBinding *binding = data;
	LOG_EVENT("key pressed: tap=%d hold=%d", binding->tap_action, binding->hold_action);
	if (binding->hold_action != ACTION_NONE) {
		clock_gettime(CLOCK_MONOTONIC, &binding->press_time);
		binding->held = true;
		binding->hold_fired = false;
	} else {
		binding->seat->pending_action = binding->tap_action;
	}
}

static void xkb_binding_handle_released(void *data, struct river_xkb_binding_v1 *obj) {
	struct XkbBinding *binding = data;
	LOG_EVENT("key released: tap=%d hold=%d held=%d hold_fired=%d",
		binding->tap_action, binding->hold_action, binding->held, binding->hold_fired);
	if (!binding->held) return;
	binding->held = false;
	if (!binding->hold_fired) {
		binding->seat->pending_action = binding->tap_action;
	}
}

static const struct river_xkb_binding_v1_listener river_xkb_binding_listener = {
	.pressed = xkb_binding_handle_pressed,
	.released = xkb_binding_handle_released,
};

static void xkb_binding_destroy(struct XkbBinding *binding) {
	river_xkb_binding_v1_destroy(binding->obj);
	wl_list_remove(&binding->link);
	free(binding);
}

static void xkb_binding_create(struct Seat *seat, uint32_t mods, xkb_keysym_t keysym,
		enum Action tap_action, enum Action hold_action) {
	struct XkbBinding *binding = calloc(1, sizeof(struct XkbBinding));
	binding->obj = river_xkb_bindings_v1_get_xkb_binding(xkb_bindings_v1, seat->obj, keysym, mods);
	binding->seat = seat;
	binding->tap_action = tap_action;
	binding->hold_action = hold_action;
	binding->held = false;
	binding->hold_fired = false;
	river_xkb_binding_v1_add_listener(binding->obj, &river_xkb_binding_listener, binding);
	river_xkb_binding_v1_enable(binding->obj);
	wl_list_insert(seat->xkb_bindings.prev, &binding->link);
}

// --- Pointer binding handlers ---

static void pointer_binding_handle_pressed(void *data, struct river_pointer_binding_v1 *obj) {
	struct PointerBinding *binding = data;
	binding->seat->pending_action = binding->action;
}

static void pointer_binding_handle_released(void *data, struct river_pointer_binding_v1 *obj) {}

static const struct river_pointer_binding_v1_listener river_pointer_binding_listener = {
	.pressed = pointer_binding_handle_pressed,
	.released = pointer_binding_handle_released,
};

static void pointer_binding_destroy(struct PointerBinding *binding) {
	river_pointer_binding_v1_destroy(binding->obj);
	wl_list_remove(&binding->link);
	free(binding);
}

static void pointer_binding_create(struct Seat *seat, uint32_t mods, uint32_t button, enum Action action) {
	struct PointerBinding *binding = calloc(1, sizeof(struct PointerBinding));
	binding->obj = river_seat_v1_get_pointer_binding(seat->obj, button, mods);
	binding->seat = seat;
	binding->action = action;
	river_pointer_binding_v1_add_listener(binding->obj, &river_pointer_binding_listener, binding);
	river_pointer_binding_v1_enable(binding->obj);
	wl_list_insert(seat->pointer_bindings.prev, &binding->link);
}

// --- Hold timer processing ---

static void seat_action(struct Seat *seat, enum Action action); // forward decl

// Returns ms until next hold fires, or -1 if no held bindings.
static int compute_poll_timeout(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int timeout = -1;
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->removed) continue;
		struct XkbBinding *binding;
		wl_list_for_each(binding, &seat->xkb_bindings, link) {
			if (!binding->held || binding->hold_fired) continue;
			long elapsed_ms = (now.tv_sec - binding->press_time.tv_sec) * 1000L
				+ (now.tv_nsec - binding->press_time.tv_nsec) / 1000000L;
			long remaining = HOLD_THRESHOLD_MS - elapsed_ms;
			if (remaining <= 0) remaining = 0;
			int r = (int)remaining;
			if (timeout == -1 || r < timeout) timeout = r;
		}
	}
	return timeout;
}

static void process_hold_timers(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->removed) continue;
		struct XkbBinding *binding;
		wl_list_for_each(binding, &seat->xkb_bindings, link) {
			if (!binding->held || binding->hold_fired) continue;
			long elapsed_ms = (now.tv_sec - binding->press_time.tv_sec) * 1000L
				+ (now.tv_nsec - binding->press_time.tv_nsec) / 1000000L;
			if (elapsed_ms >= HOLD_THRESHOLD_MS) {
				binding->hold_fired = true;
				LOG_EVENT("hold fired: action=%d (elapsed=%ldms)", binding->hold_action, elapsed_ms);
				// Chama diretamente — não via pending_action — porque manage_start
				// só chega quando o usuário solta a tecla, o que causaria o hold
				// parecer executar só no release.
				seat_action(seat, binding->hold_action);
				clamp_target();
			}
		}
	}
}

// --- Seat handlers ---

static void seat_handle_removed(void *data, struct river_seat_v1 *obj) {
	struct Seat *seat = data;
	seat->removed = true;
}

static void seat_handle_pointer_enter(void *data, struct river_seat_v1 *obj, struct river_window_v1 *river_window) {
	struct Seat *seat = data;
	seat->hovered = river_window_v1_get_user_data(river_window);
}

static void seat_handle_pointer_leave(void *data, struct river_seat_v1 *obj) {
	struct Seat *seat = data;
	seat->hovered = NULL;
}

static void seat_handle_window_interaction(void *data, struct river_seat_v1 *obj, struct river_window_v1 *river_window) {
	struct Seat *seat = data;
	seat->interacted = river_window_v1_get_user_data(river_window);
	// Focus went to a normal window → dismiss the launcher if it's open.
	close_launcher();
}

static void seat_handle_wl_seat(void *data, struct river_seat_v1 *obj, uint32_t id) {}
// Clicking the layer surface itself (the fuzzel window) — do NOT dismiss.
static void seat_handle_shell_surface_interaction(void *data, struct river_seat_v1 *obj, struct river_shell_surface_v1 *river_shell_surface) {}
static void seat_handle_op_delta(void *data, struct river_seat_v1 *obj, int32_t dx, int32_t dy) {}
static void seat_handle_op_release(void *data, struct river_seat_v1 *obj) {}
static void seat_handle_pointer_position(void *data, struct river_seat_v1 *obj, int32_t x, int32_t y) {}

static const struct river_seat_v1_listener river_seat_listener = {
	.removed = seat_handle_removed,
	.wl_seat = seat_handle_wl_seat,
	.pointer_enter = seat_handle_pointer_enter,
	.pointer_leave = seat_handle_pointer_leave,
	.window_interaction = seat_handle_window_interaction,
	.shell_surface_interaction = seat_handle_shell_surface_interaction,
	.op_delta = seat_handle_op_delta,
	.op_release = seat_handle_op_release,
	.pointer_position = seat_handle_pointer_position,
};

static void seat_maybe_destroy(struct Seat *seat) {
	if (!seat->removed) return;
	struct XkbBinding *xkb_binding, *xkb_binding_tmp;
	wl_list_for_each_safe(xkb_binding, xkb_binding_tmp, &seat->xkb_bindings, link) {
		xkb_binding_destroy(xkb_binding);
	}
	struct PointerBinding *pointer_binding, *pointer_binding_tmp;
	wl_list_for_each_safe(pointer_binding, pointer_binding_tmp, &seat->pointer_bindings, link) {
		pointer_binding_destroy(pointer_binding);
	}
	river_seat_v1_destroy(seat->obj);
	wl_list_remove(&seat->link);
	free(seat);
}

static void seat_action(struct Seat *seat, enum Action action) {
	switch (action) {
	case ACTION_NONE:
		break;
	case ACTION_SPAWN_TERMINAL:
		spawn_command("foot");
		break;
	case ACTION_SPAWN_LAUNCHER:
		// spawn_sh (sh -c) — NOT spawn_command: the latter execlp's the whole
		// string as one argv[0] (a "program" with spaces) → ENOENT → never runs.
		spawn_sh("bash /home/tcfialho/.config/niri/fuzzel-toggle.sh");
		clock_gettime(CLOCK_MONOTONIC, &last_launcher_spawn);
		last_launcher_spawn_valid = true;
		break;
	case ACTION_CLOSE_TARGET: {
		struct Window *target = target_window();
		if (target != NULL) {
			osd2(window_label(target), " fechado");
			river_window_v1_close(target->obj);
			wm.maximized = false;
		}
		break;
	}
	case ACTION_TOGGLE_TARGET:
		if (window_count() >= 2) {
			wm.target_index = wm.target_index == 0 ? 1 : 0;
			wm.maximized = false;
			osd(wm.target_index == 0 ? "alvo: MAIN" : "alvo: DECK");
		}
		break;
	case ACTION_SWAP_MAIN_DECK:
		md_swap_main_deck();
		break;
	case ACTION_DECK_NEXT:
		md_deck_next();
		break;
	case ACTION_DECK_PREV:
		md_deck_prev();
		break;
	case ACTION_SEND_TARGET_TO_DECK_BOTTOM:
		md_send_target_to_deck_bottom();
		break;
	case ACTION_PROMOTE_TARGET_TO_MAIN:
		md_promote_target_to_main();
		break;
	case ACTION_MAXIMIZE_TARGET: {
		struct Window *target = target_window();
		if (target != NULL && !wm.maximized) {
			wm.maximized = true;
			char buf[256];
			snprintf(buf, sizeof(buf), "%s em MAX", window_label(target));
			osd(buf);
		}
		break;
	}
	case ACTION_RESTORE:
		if (wm.maximized) {
			wm.maximized = false;
			osd("restaurado");
		}
		break;
	case ACTION_EXIT:
		// Restart the display manager (SDDM) so it shows the greeter cleanly.
		// loginctl terminate-session alone leaves a black screen with SDDM autologin.
		spawn_sh("sudo -n systemctl restart sddm 2>/dev/null || loginctl terminate-session \"$XDG_SESSION_ID\"");
		break;
	}
}

static void seat_manage(struct Seat *seat) {
	if (seat->new) {
		seat->new = false;
		LOG_EVENT("seat init: creating xkb bindings");
		const uint32_t super = RIVER_SEAT_V1_MODIFIERS_MOD4;
		const uint32_t ctrl  = RIVER_SEAT_V1_MODIFIERS_CTRL;
		const uint32_t alt   = RIVER_SEAT_V1_MODIFIERS_MOD1;

		// --- Tap/hold via keyd (the real input path on this machine) ---
		// keyd's [meta] layer (active while Super is held) resolves tap vs hold
		// itself and emits DIFFERENT F-keys to the compositor — Super is NOT
		// passed through. So River never sees Super+Tab/Left/Right; it sees
		// these F-key combos. We mirror niri's config.kdl exactly (keyd runs
		// below the compositor, so the mapping is identical for both).
		// Each is tap-only from our side (keyd already decided tap/hold).
		//   Win+Tab tap=Alt+F23  hold=Alt+Ctrl+F23
		//   Win+←   tap=F23       hold=Ctrl+F23
		//   Win+→   tap=F24       hold=Ctrl+F24
		xkb_binding_create(seat, alt,        XKB_KEY_F23, ACTION_TOGGLE_TARGET,             ACTION_NONE);
		xkb_binding_create(seat, alt | ctrl, XKB_KEY_F23, ACTION_SWAP_MAIN_DECK,            ACTION_NONE);
		xkb_binding_create(seat, 0,          XKB_KEY_F23, ACTION_DECK_PREV,                 ACTION_NONE);
		xkb_binding_create(seat, ctrl,       XKB_KEY_F23, ACTION_PROMOTE_TARGET_TO_MAIN,    ACTION_NONE);
		xkb_binding_create(seat, 0,          XKB_KEY_F24, ACTION_DECK_NEXT,                 ACTION_NONE);
		xkb_binding_create(seat, ctrl,       XKB_KEY_F24, ACTION_SEND_TARGET_TO_DECK_BOTTOM,ACTION_NONE);

		// Insurance: also bind the raw Super+Tab/Left/Right with our own
		// tap/hold timer, for the case where keyd is NOT running (then these
		// keysyms reach River directly). When keyd IS active these never fire
		// (it emits F23/F24 instead), so they're harmless duplicates.
		xkb_binding_create(seat, super, XKB_KEY_Tab,   ACTION_TOGGLE_TARGET, ACTION_SWAP_MAIN_DECK);
		xkb_binding_create(seat, super, XKB_KEY_Right, ACTION_DECK_NEXT,     ACTION_SEND_TARGET_TO_DECK_BOTTOM);
		xkb_binding_create(seat, super, XKB_KEY_Left,  ACTION_DECK_PREV,     ACTION_PROMOTE_TARGET_TO_MAIN);

		// Tap-only bindings:
		// F19 is emitted by keyd on lone Super tap (overload(meta, f19))
		xkb_binding_create(seat, 0, XKB_KEY_F19, ACTION_SPAWN_LAUNCHER, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Return, ACTION_SPAWN_TERMINAL, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Delete, ACTION_CLOSE_TARGET, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Up, ACTION_MAXIMIZE_TARGET, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Down, ACTION_RESTORE, ACTION_NONE);
		xkb_binding_create(seat, super | RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_Escape, ACTION_EXIT, ACTION_NONE);

		// Super+click: toggle ALVO between MAIN and DECK
		pointer_binding_create(seat, super, BTN_LEFT, ACTION_TOGGLE_TARGET);
	}

	// window_interaction chega em todo manage_start para a janela atualmente focada
	// (não só em cliques). Só mudamos ALVO se o usuário clicou numa janela diferente
	// da que já estava focada — comparar com seat->focused (foco do ciclo anterior)
	// distingue "click numa janela nova" de "compositor reportando foco atual".
	if (seat->interacted != NULL && seat->interacted != seat->focused) {
		int32_t idx = window_index(seat->interacted);
		if (idx == 1) {
			// Clicked DECK window → make it the ALVO (don't promote)
			wm.target_index = 1;
			wm.maximized = false;
		} else if (idx == 0) {
			wm.target_index = 0;
			wm.maximized = false;
		}
	}
	seat->interacted = NULL;

	seat_action(seat, seat->pending_action);
	seat->pending_action = ACTION_NONE;
}

// --- Window manager handlers ---

static void wm_handle_unavailable(void *data, struct river_window_manager_v1 *obj) {
	LOG_WARN("another window manager is already running");
	exit(1);
}

static void wm_handle_finished(void *data, struct river_window_manager_v1 *obj) {
	LOG_INFO("session finished");
	exit(0);
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

	// Also process any fired hold actions (from poll timeout between manage cycles)
	process_hold_timers();

	wl_list_for_each(seat, &wm.seats, link) {
		seat_manage(seat);
	}
	clamp_target();

	// Set default layer-shell output (must be done inside manage sequence)
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

	size_t index = 0;
	wl_list_for_each(window, &wm.windows, link) {
		window_manage_layout(window, index);
		index++;
	}
	focus_target_on_seats();
	log_state();

	river_window_manager_v1_manage_finish(window_manager_v1);
}

static void wm_handle_render_start(void *data, struct river_window_manager_v1 *obj) {
	size_t index = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		window_render_layout(window, index);
		index++;
	}
	if (target_window() != NULL) {
		river_node_v1_place_top(target_window()->node);
	}
	river_window_manager_v1_render_finish(window_manager_v1);
}

static void wm_handle_window(void *data, struct river_window_manager_v1 *obj, struct river_window_v1 *river_window) {
	struct Window *window = calloc(1, sizeof(struct Window));
	if (window == NULL) {
		LOG_WARN("calloc(Window) failed; dropping window (OOM)");
		return; /* never crash the sole window manager on OOM */
	}
	window->obj = river_window;
	window->node = river_window_v1_get_node(window->obj);
	window->new = true;
	river_window_v1_add_listener(window->obj, &river_window_listener, window);
	md_insert_new_window(window);
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
}

static void wm_handle_session_locked(void *data, struct river_window_manager_v1 *obj) {}
static void wm_handle_session_unlocked(void *data, struct river_window_manager_v1 *obj) {}

static const struct river_window_manager_v1_listener wm_listener = {
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

static void wm_init(void) {
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
			window_manager_v1 = wl_registry_bind(registry, name, &river_window_manager_v1_interface, 4);
		}
	} else if (strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
		xkb_bindings_v1 = wl_registry_bind(registry, name, &river_xkb_bindings_v1_interface, 1);
	} else if (strcmp(interface, river_layer_shell_v1_interface.name) == 0) {
		// Binding signals to the compositor that we accept layer-shell clients
		// (waybar, wofi, etc). Without this, River closes their layer surfaces
		// immediately. See river-layer-shell-v1.xml.
		layer_shell_v1 = wl_registry_bind(registry, name, &river_layer_shell_v1_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to connect to Wayland server\n");
		return 1;
	}

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
	LOG_INFO("globals bound: window_manager=%p xkb_bindings=%p layer_shell=%p",
		(void *)window_manager_v1, (void *)xkb_bindings_v1, (void *)layer_shell_v1);
	if (layer_shell_v1 == NULL) {
		LOG_WARN("river_layer_shell_v1 not advertised — layer-shell clients (waybar) will be closed by the compositor");
	}

	wm_init();
	river_window_manager_v1_add_listener(window_manager_v1, &wm_listener, NULL);

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
		struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
		int ret = poll(&pfd, 1, timeout_ms);

		if (ret < 0) {
			wl_display_cancel_read(display);
			break;
		}

		if (pfd.revents & POLLIN) {
			wl_display_read_events(display);
			if (wl_display_dispatch_pending(display) < 0) break;
		} else {
			// Timeout: a hold threshold was reached
			wl_display_cancel_read(display);
			process_hold_timers();
			if (wl_display_dispatch_pending(display) < 0) break;
		}
	}

done:
	return 0;
}
