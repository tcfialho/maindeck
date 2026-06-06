#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <errno.h>
#include <limits.h>

#include "types.h"
#include "wm-log.h"
#include "wm-state.h"
#include "wm-layout.h"
#include "wm-input.h"
#include "cursor-shape-v1-client-protocol.h"

#define HOLD_THRESHOLD_MS 360
#define DOUBLE_TAP_MS 280

static struct timespec last_launcher_spawn;
static bool last_launcher_spawn_valid = false;

void close_launcher(void) {
	if (last_launcher_spawn_valid) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long since_ms = (now.tv_sec - last_launcher_spawn.tv_sec) * 1000L
			+ (now.tv_nsec - last_launcher_spawn.tv_nsec) / 1000000L;
		if (since_ms < 250) return; // ignore the open's own focus settling
	}
	if (fork() == 0) {
		execlp("pkill", "pkill", "-x", "maindeck-menu", (char *)0);
		_exit(127);
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
		return;
	}

	// Win+Tab double-tap → cycle the DECK. The first tap fires the normal
	// ACTION_TOGGLE_TARGET immediately (no latency on the common action). A
	// second toggle-tap within DOUBLE_TAP_MS runs DECK_NEXT. We deliberately do
	// NOT undo the first tap's toggle: the ALVO simply lands on DECK and stays
	// (no flash-and-restore), trading exact "like Win+→" parity for zero latency
	// and no flicker.
	if (binding->tap_action == ACTION_TOGGLE_TARGET) {
		static struct timespec last_toggle_tap;
		static bool last_toggle_tap_valid = false;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long since_ms = last_toggle_tap_valid
			? (now.tv_sec - last_toggle_tap.tv_sec) * 1000L
				+ (now.tv_nsec - last_toggle_tap.tv_nsec) / 1000000L
			: -1;
		if (last_toggle_tap_valid && since_ms >= 0 && since_ms <= DOUBLE_TAP_MS) {
			// Second quick tap: cycle the DECK (don't undo the first toggle).
			binding->seat->pending_action = ACTION_DECK_NEXT;
			last_toggle_tap_valid = false; // a third tap starts fresh
			return;
		}
		// First tap: fire the toggle now and arm the double-tap window.
		last_toggle_tap = now;
		last_toggle_tap_valid = true;
		binding->seat->pending_action = ACTION_TOGGLE_TARGET;
		return;
	}

	binding->seat->pending_action = binding->tap_action;
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

int compute_poll_timeout(void) {
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

void process_hold_timers(void) {
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
				seat_action(seat, binding->hold_action);
				clamp_target();
			}
		}
	}
}

// --- Seat cursor fallback ---

static void wm_pointer_enter(void *data, struct wl_pointer *ptr,
		uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)ptr; (void)serial; (void)surface; (void)sx; (void)sy;
}
static void wm_pointer_leave(void *data, struct wl_pointer *ptr,
		uint32_t serial, struct wl_surface *surface) {
	(void)data; (void)ptr; (void)serial; (void)surface;
}
static void wm_pointer_motion(void *data, struct wl_pointer *ptr,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
	(void)data; (void)ptr; (void)time; (void)sx; (void)sy;
}
static void wm_pointer_button(void *data, struct wl_pointer *ptr,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	(void)data; (void)ptr; (void)serial; (void)time; (void)button; (void)state;
}
static void wm_pointer_axis(void *data, struct wl_pointer *ptr,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	(void)data; (void)ptr; (void)time; (void)axis; (void)value;
}
static void wm_pointer_frame(void *data, struct wl_pointer *ptr) {
	(void)data; (void)ptr;
}
static void wm_pointer_axis_source(void *data, struct wl_pointer *ptr, uint32_t source) {
	(void)data; (void)ptr; (void)source;
}
static void wm_pointer_axis_stop(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis) {
	(void)data; (void)ptr; (void)time; (void)axis;
}
static void wm_pointer_axis_discrete(void *data, struct wl_pointer *ptr, uint32_t axis, int32_t discrete) {
	(void)data; (void)ptr; (void)axis; (void)discrete;
}
static void wm_pointer_axis_value120(void *data, struct wl_pointer *ptr, uint32_t axis, int32_t value120) {
	(void)data; (void)ptr; (void)axis; (void)value120;
}
static void wm_pointer_axis_relative_direction(void *data, struct wl_pointer *ptr, uint32_t axis, uint32_t direction) {
	(void)data; (void)ptr; (void)axis; (void)direction;
}

static const struct wl_pointer_listener wm_pointer_listener = {
	.enter = wm_pointer_enter,
	.leave = wm_pointer_leave,
	.motion = wm_pointer_motion,
	.button = wm_pointer_button,
	.axis = wm_pointer_axis,
	.frame = wm_pointer_frame,
	.axis_source = wm_pointer_axis_source,
	.axis_stop = wm_pointer_axis_stop,
	.axis_discrete = wm_pointer_axis_discrete,
	.axis_value120 = wm_pointer_axis_value120,
	.axis_relative_direction = wm_pointer_axis_relative_direction,
};

static uint32_t cursor_size_from_env(void) {
	const char *value = getenv("XCURSOR_SIZE");
	if (value == NULL || value[0] == '\0') return 24;

	errno = 0;
	char *end = NULL;
	unsigned long parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 512) {
		return 24;
	}
	return (uint32_t)parsed;
}

static void seat_apply_xcursor_theme(struct Seat *seat) {
	if (river_seat_v1_get_version(seat->obj) < RIVER_SEAT_V1_SET_XCURSOR_THEME_SINCE_VERSION) {
		return;
	}
	const char *theme = getenv("XCURSOR_THEME");
	if (theme == NULL) theme = "";
	river_seat_v1_set_xcursor_theme(seat->obj, theme, cursor_size_from_env());
}

static void seat_set_default_cursor(struct Seat *seat) {
	if (seat->cursor_shape_device == NULL) return;
	wp_cursor_shape_device_v1_set_shape(
		seat->cursor_shape_device,
		0,
		WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static void seat_ensure_cursor_shape_device(struct Seat *seat) {
	if (seat->wl_pointer == NULL ||
	    seat->cursor_shape_device != NULL ||
	    cursor_shape_manager_v1 == NULL) {
		return;
	}
	seat->cursor_shape_device =
		wp_cursor_shape_manager_v1_get_pointer(cursor_shape_manager_v1, seat->wl_pointer);
	seat_set_default_cursor(seat);
}

static void wm_wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t caps) {
	struct Seat *seat = data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && seat->wl_pointer == NULL) {
		seat->wl_pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(seat->wl_pointer, &wm_pointer_listener, seat);
		seat_ensure_cursor_shape_device(seat);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && seat->wl_pointer != NULL) {
		if (seat->cursor_shape_device != NULL) {
			wp_cursor_shape_device_v1_destroy(seat->cursor_shape_device);
			seat->cursor_shape_device = NULL;
		}
		wl_pointer_destroy(seat->wl_pointer);
		seat->wl_pointer = NULL;
	}
}

static void wm_wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
	(void)data; (void)wl_seat; (void)name;
}

static const struct wl_seat_listener wm_wl_seat_listener = {
	.capabilities = wm_wl_seat_capabilities,
	.name = wm_wl_seat_name,
};

static void seat_bind_wl_seat(struct Seat *seat, uint32_t name) {
	seat->wl_seat_name = name;
	if (seat->wl_seat != NULL || wm_registry == NULL) return;

	seat->wl_seat = wl_registry_bind(wm_registry, name, &wl_seat_interface, 1);
	if (seat->wl_seat != NULL) {
		wl_seat_add_listener(seat->wl_seat, &wm_wl_seat_listener, seat);
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
	close_launcher();
}

static void seat_handle_wl_seat(void *data, struct river_seat_v1 *obj, uint32_t id) {
	struct Seat *seat = data;
	(void)obj;
	seat_bind_wl_seat(seat, id);
}
static void seat_handle_shell_surface_interaction(void *data, struct river_seat_v1 *obj, struct river_shell_surface_v1 *river_shell_surface) {}
static void seat_handle_op_delta(void *data, struct river_seat_v1 *obj, int32_t dx, int32_t dy) {}
static void seat_handle_op_release(void *data, struct river_seat_v1 *obj) {}
static void seat_handle_pointer_position(void *data, struct river_seat_v1 *obj, int32_t x, int32_t y) {}

const struct river_seat_v1_listener river_seat_listener = {
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

void seat_maybe_destroy(struct Seat *seat) {
	if (!seat->removed) return;
	struct XkbBinding *xkb_binding, *xkb_binding_tmp;
	wl_list_for_each_safe(xkb_binding, xkb_binding_tmp, &seat->xkb_bindings, link) {
		xkb_binding_destroy(xkb_binding);
	}
	struct PointerBinding *pointer_binding, *pointer_binding_tmp;
	wl_list_for_each_safe(pointer_binding, pointer_binding_tmp, &seat->pointer_bindings, link) {
		pointer_binding_destroy(pointer_binding);
	}
	if (seat->cursor_shape_device != NULL) {
		wp_cursor_shape_device_v1_destroy(seat->cursor_shape_device);
		seat->cursor_shape_device = NULL;
	}
	if (seat->wl_pointer != NULL) {
		wl_pointer_destroy(seat->wl_pointer);
		seat->wl_pointer = NULL;
	}
	if (seat->wl_seat != NULL) {
		wl_seat_destroy(seat->wl_seat);
		seat->wl_seat = NULL;
	}
	if (seat->layer_shell_seat != NULL) {
		river_layer_shell_seat_v1_destroy(seat->layer_shell_seat);
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
		spawn_command("kitty");
		break;
	case ACTION_SPAWN_LAUNCHER:
		spawn_command("maindeck-menu");
		clock_gettime(CLOCK_MONOTONIC, &last_launcher_spawn);
		last_launcher_spawn_valid = true;
		break;
	case ACTION_CLOSE_TARGET: {
		struct Window *target = target_window();
		if (target != NULL) {
			river_window_v1_close(target->obj);
			wm.maximized = false;
		}
		break;
	}
	case ACTION_TOGGLE_TARGET:
		if (visible_window_count() >= 2) {
			wm.target_index = wm.target_index == 0 ? 1 : 0;
			wm.maximized = false;
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
		}
		break;
	}
	case ACTION_RESTORE:
		if (wm.maximized) {
			wm.maximized = false;
		}
		break;
	case ACTION_TOGGLE_MAXIMIZE:
		// Win+Shift (tap): keyd emite Ctrl+F19. Alterna maximize/restore.
		if (target_window() != NULL) {
			wm.maximized = !wm.maximized;
		}
		break;
	case ACTION_EXIT:
		spawn_sh("sudo -n systemctl restart sddm 2>/dev/null || loginctl terminate-session \"$XDG_SESSION_ID\"");
		break;
	}
}

void seat_manage(struct Seat *seat) {
	if (seat->new) {
		seat->new = false;
		LOG_EVENT("seat init: creating xkb bindings");
		seat_apply_xcursor_theme(seat);
		seat_ensure_cursor_shape_device(seat);
		const uint32_t super = RIVER_SEAT_V1_MODIFIERS_MOD4;
		const uint32_t ctrl  = RIVER_SEAT_V1_MODIFIERS_CTRL;
		const uint32_t alt   = RIVER_SEAT_V1_MODIFIERS_MOD1;

		xkb_binding_create(seat, alt,        XKB_KEY_F23, ACTION_TOGGLE_TARGET,             ACTION_NONE);
		xkb_binding_create(seat, alt | ctrl, XKB_KEY_F23, ACTION_SWAP_MAIN_DECK,            ACTION_NONE);
		xkb_binding_create(seat, 0,          XKB_KEY_F23, ACTION_DECK_PREV,                 ACTION_NONE);
		xkb_binding_create(seat, ctrl,       XKB_KEY_F23, ACTION_PROMOTE_TARGET_TO_MAIN,    ACTION_NONE);
		xkb_binding_create(seat, 0,          XKB_KEY_F24, ACTION_DECK_NEXT,                 ACTION_NONE);
		xkb_binding_create(seat, ctrl,       XKB_KEY_F24, ACTION_SEND_TARGET_TO_DECK_BOTTOM,ACTION_NONE);

		xkb_binding_create(seat, super, XKB_KEY_Tab,   ACTION_TOGGLE_TARGET, ACTION_SWAP_MAIN_DECK);
		xkb_binding_create(seat, super, XKB_KEY_Right, ACTION_DECK_NEXT,     ACTION_SEND_TARGET_TO_DECK_BOTTOM);
		xkb_binding_create(seat, super, XKB_KEY_Left,  ACTION_DECK_PREV,     ACTION_PROMOTE_TARGET_TO_MAIN);

		xkb_binding_create(seat, 0, XKB_KEY_F19, ACTION_SPAWN_LAUNCHER, ACTION_NONE);
		// Win+Shift (tap, sem outra tecla) → keyd emite Ctrl+F19: toggle maximize.
		// (F14 não faz round-trip neste keymap; F19 sim — é o keysym do launcher.)
		xkb_binding_create(seat, ctrl, XKB_KEY_F19, ACTION_TOGGLE_MAXIMIZE, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Return, ACTION_SPAWN_TERMINAL, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Delete, ACTION_CLOSE_TARGET, ACTION_NONE);
		xkb_binding_create(seat, alt, XKB_KEY_F4, ACTION_CLOSE_TARGET, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Up, ACTION_MAXIMIZE_TARGET, ACTION_NONE);
		xkb_binding_create(seat, super, XKB_KEY_Down, ACTION_RESTORE, ACTION_NONE);
		xkb_binding_create(seat, super | RIVER_SEAT_V1_MODIFIERS_SHIFT, XKB_KEY_Escape, ACTION_EXIT, ACTION_NONE);

		pointer_binding_create(seat, super, BTN_LEFT, ACTION_TOGGLE_TARGET);
	}

	if (seat->interacted != NULL) {
		int32_t idx = window_index(seat->interacted);
		if (idx == 0 || idx == 1) {
			if (idx != (int32_t)wm.target_index) {
				wm.target_index = (uint32_t)idx;
				wm.maximized = false;
			}
			wm.focus_dirty = true;
		}
	}
	seat->interacted = NULL;

	seat_action(seat, seat->pending_action);
	seat->pending_action = ACTION_NONE;
}
