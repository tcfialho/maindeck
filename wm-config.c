#define JSMN_STATIC
#include "../vendor/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "wm-config.h"
#include "wm-log.h"

struct WmConfig g_wm_config = {0};

static int tok_eq(const char *json, jsmntok_t *tok, const char *s) {
	int len = tok->end - tok->start;
	return (int)strlen(s) == len && memcmp(json + tok->start, s, (size_t)len) == 0;
}

void wm_config_load(void) {
	// Clean up old config if any
	wm_config_free();
	g_wm_config.force_tearing_fullscreen = false;

	bool has_env_override = false;
	const char *env = getenv("MAINDECK_FORCE_TEARING");
	if (env != NULL) {
		if (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0) {
			g_wm_config.force_tearing_fullscreen = true;
			has_env_override = true;
		} else if (strcmp(env, "0") == 0 || strcasecmp(env, "false") == 0) {
			g_wm_config.force_tearing_fullscreen = false;
			has_env_override = true;
		}
	}

	char path[512];
	const char *config_home = getenv("XDG_CONFIG_HOME");
	if (config_home != NULL && config_home[0] != '\0') {
		snprintf(path, sizeof(path), "%s/maindeck/wm.json", config_home);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL) return;
		snprintf(path, sizeof(path), "%s/.config/maindeck/wm.json", home);
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		LOG_INFO("wm-config: no config file found at %s, using defaults", path);
		return;
	}

	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size == 0) {
		close(fd);
		return;
	}

	char *buf = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (buf == MAP_FAILED) return;

	jsmn_parser p;
	jsmn_init(&p);
	// 512 tokens are enough for a simple config file
	jsmntok_t *tokens = malloc(sizeof(jsmntok_t) * 512);
	if (tokens == NULL) {
		munmap(buf, (size_t)st.st_size);
		return;
	}

	int r = jsmn_parse(&p, buf, (size_t)st.st_size, tokens, 512);
	if (r < 0) {
		LOG_WARN("wm-config: failed to parse JSON: %d", r);
		free(tokens);
		munmap(buf, (size_t)st.st_size);
		return;
	}

	if (r < 1 || tokens[0].type != JSMN_OBJECT) {
		LOG_WARN("wm-config: top-level JSON element must be an object");
		free(tokens);
		munmap(buf, (size_t)st.st_size);
		return;
	}

	for (int i = 1; i < r; i++) {
		if (tokens[i].type == JSMN_STRING && tok_eq(buf, &tokens[i], "floating_app_ids")) {
			if (i + 1 < r && tokens[i + 1].type == JSMN_ARRAY) {
				int arr_len = tokens[i + 1].size;
				int next_idx = i + 2;
				for (int j = 0; j < arr_len && g_wm_config.floating_app_ids_count < MAX_FLOAT_APPS; j++) {
					if (next_idx < r && tokens[next_idx].type == JSMN_STRING) {
						int len = tokens[next_idx].end - tokens[next_idx].start;
						char *str = malloc((size_t)len + 1);
						if (str != NULL) {
							memcpy(str, buf + tokens[next_idx].start, (size_t)len);
							str[len] = '\0';
							g_wm_config.floating_app_ids[g_wm_config.floating_app_ids_count++] = str;
						}
					}
					next_idx++;
				}
				i = next_idx - 1;
			}
		} else if (tokens[i].type == JSMN_STRING && tok_eq(buf, &tokens[i], "force_tearing_fullscreen")) {
			if (i + 1 < r && (tokens[i + 1].type == JSMN_PRIMITIVE)) {
				if (!has_env_override) {
					int len = tokens[i + 1].end - tokens[i + 1].start;
					if (len == 4 && memcmp(buf + tokens[i + 1].start, "true", 4) == 0) {
						g_wm_config.force_tearing_fullscreen = true;
					} else {
						g_wm_config.force_tearing_fullscreen = false;
					}
				}
				i++;
			}
		}
	}

	free(tokens);
	munmap(buf, (size_t)st.st_size);
	LOG_INFO("wm-config: loaded %d floating app_ids from %s", g_wm_config.floating_app_ids_count, path);
}

bool wm_config_should_float(const char *app_id) {
	if (app_id == NULL) return false;
	for (int i = 0; i < g_wm_config.floating_app_ids_count; i++) {
		if (strcasecmp(app_id, g_wm_config.floating_app_ids[i]) == 0) {
			return true;
		}
	}
	return false;
}

void wm_config_free(void) {
	for (int i = 0; i < g_wm_config.floating_app_ids_count; i++) {
		free(g_wm_config.floating_app_ids[i]);
		g_wm_config.floating_app_ids[i] = NULL;
	}
	g_wm_config.floating_app_ids_count = 0;
}
