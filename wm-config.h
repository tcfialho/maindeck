#ifndef WM_CONFIG_H
#define WM_CONFIG_H

#include <stdbool.h>

#define MAX_FLOAT_APPS 64

struct WmConfig {
	char *floating_app_ids[MAX_FLOAT_APPS];
	int floating_app_ids_count;
	bool force_tearing_fullscreen;
	char *screenshot_command;
};

void wm_config_load(void);
bool wm_config_should_float(const char *app_id);
void wm_config_free(void);

extern struct WmConfig g_wm_config;

#endif /* WM_CONFIG_H */
