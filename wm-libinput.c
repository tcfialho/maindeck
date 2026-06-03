#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wayland-client-core.h>

#include <river-libinput-config-v1-client-protocol.h>
#include <river-input-management-v1-client-protocol.h>

#include "types.h"
#include "wm-log.h"
#include "wm-state.h"
#include "wm-libinput.h"

static void result_handle_success(void *data, struct river_libinput_result_v1 *obj) {
	LOG_INFO("libinput config applied successfully");
}

static void result_handle_unsupported(void *data, struct river_libinput_result_v1 *obj) {
	LOG_WARN("libinput config unsupported by device");
}

static void result_handle_invalid(void *data, struct river_libinput_result_v1 *obj) {
	LOG_WARN("libinput config invalid");
}

static const struct river_libinput_result_v1_listener result_listener = {
	.success = result_handle_success,
	.unsupported = result_handle_unsupported,
	.invalid = result_handle_invalid,
};

static void set_device_accel_speed(struct river_libinput_device_v1 *device, double speed) {
	struct wl_array arr;
	wl_array_init(&arr);
	double *p = wl_array_add(&arr, sizeof(double));
	if (p) {
		*p = speed;
		struct river_libinput_result_v1 *res = river_libinput_device_v1_set_accel_speed(device, &arr);
		river_libinput_result_v1_add_listener(res, &result_listener, NULL);
		LOG_INFO("Sent set_accel_speed request with value: %f", speed);
	}
	wl_array_release(&arr);
}

static void input_device_handle_removed(void *data, struct river_input_device_v1 *obj) {
	// no-op, handled by libinput device removed event
}

static void input_device_handle_type(void *data, struct river_input_device_v1 *obj, uint32_t type) {
	struct DeviceState *state = data;
	state->type = type;
}

static void input_device_handle_name(void *data, struct river_input_device_v1 *obj, const char *name) {
	struct DeviceState *state = data;
	state->name = strdup(name);
	LOG_INFO("Input device name: %s, type: %u", name, state->type);
	
	char *lower = strdup(name);
	if (lower) {
		for (int i = 0; lower[i]; i++) {
			lower[i] = tolower((unsigned char)lower[i]);
		}
		if (strstr(lower, "touchpad") != NULL) {
			state->is_touchpad = true;
			LOG_INFO("Identified touchpad by name: %s", name);
			if (state->libinput_dev) {
				// Increase speed to 0.7 for higher sensitivity
				set_device_accel_speed(state->libinput_dev, 0.7);
			}
		}
		free(lower);
	}
}

static const struct river_input_device_v1_listener input_device_listener = {
	.removed = input_device_handle_removed,
	.type = input_device_handle_type,
	.name = input_device_handle_name,
};

static void libinput_device_handle_removed(void *data, struct river_libinput_device_v1 *obj) {
	struct DeviceState *state = data;
	LOG_INFO("libinput device removed: %s", state->name ? state->name : "unknown");
	if (state->input_dev) {
		river_input_device_v1_destroy(state->input_dev);
	}
	river_libinput_device_v1_destroy(state->libinput_dev);
	free(state->name);
	free(state);
}

static void libinput_device_handle_input_device(void *data, struct river_libinput_device_v1 *obj, struct river_input_device_v1 *device) {
	struct DeviceState *state = data;
	state->input_dev = device;
	river_input_device_v1_add_listener(device, &input_device_listener, state);
}

static void libinput_device_handle_tap_support(void *data, struct river_libinput_device_v1 *obj, int32_t finger_count) {
	struct DeviceState *state = data;
	LOG_INFO("Device %s tap support finger count: %d", state->name ? state->name : "unknown", finger_count);
	if (finger_count > 0) {
		state->has_tap_support = true;
		state->is_touchpad = true;
		LOG_INFO("Enabling tap-to-click, drag, and increasing speed on touchpad");
		
		// Enable tap-to-click
		struct river_libinput_result_v1 *res_tap = river_libinput_device_v1_set_tap(obj, 1);
		river_libinput_result_v1_add_listener(res_tap, &result_listener, NULL);
		
		// Enable tap-and-drag
		struct river_libinput_result_v1 *res_drag = river_libinput_device_v1_set_drag(obj, 1);
		river_libinput_result_v1_add_listener(res_drag, &result_listener, NULL);
		
		// Increase speed to 0.7 for higher sensitivity
		set_device_accel_speed(obj, 0.7);
	}
}

static void libinput_device_handle_dwt_support(void *data, struct river_libinput_device_v1 *obj, int32_t supported) {
	(void)data;
	if (supported) {
		LOG_INFO("Enabling disable-while-typing on touchpad");
		struct river_libinput_result_v1 *res_dwt = river_libinput_device_v1_set_dwt(obj, 1);
		river_libinput_result_v1_add_listener(res_dwt, &result_listener, NULL);
	}
}

static struct river_libinput_device_v1_listener libinput_device_listener;

static void dummy_callback(void *data, struct river_libinput_device_v1 *obj) {
	// no-op
}

void init_libinput_listeners(void) {
	void (**arr)(void) = (void (**)(void))&libinput_device_listener;
	size_t count = sizeof(struct river_libinput_device_v1_listener) / sizeof(void (*)(void));
	for (size_t i = 0; i < count; i++) {
		arr[i] = (void (*)(void))dummy_callback;
	}
	libinput_device_listener.removed = libinput_device_handle_removed;
	libinput_device_listener.input_device = libinput_device_handle_input_device;
	libinput_device_listener.tap_support = libinput_device_handle_tap_support;
	libinput_device_listener.dwt_support = libinput_device_handle_dwt_support;
}

static void libinput_config_handle_finished(void *data, struct river_libinput_config_v1 *obj) {
	// no-op
}

static void libinput_config_handle_device(void *data, struct river_libinput_config_v1 *obj, struct river_libinput_device_v1 *device) {
	struct DeviceState *state = calloc(1, sizeof(struct DeviceState));
	if (state) {
		state->libinput_dev = device;
		river_libinput_device_v1_add_listener(device, &libinput_device_listener, state);
	}
}

const struct river_libinput_config_v1_listener libinput_config_listener = {
	.finished = libinput_config_handle_finished,
	.libinput_device = libinput_config_handle_device,
};
