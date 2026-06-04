#define JSMN_STATIC
#include "../vendor/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "bar-config.h"
#include "bar-log.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int tok_eq(const char *json, jsmntok_t *tok, const char *s) {
    int len = tok->end - tok->start;
    return (int)strlen(s) == len && memcmp(json + tok->start, s, (size_t)len) == 0;
}

static void tok_str(const char *json, jsmntok_t *tok, char *dst, size_t cap) {
    size_t len = (size_t)(tok->end - tok->start);
    if (len >= cap) len = cap - 1;
    memcpy(dst, json + tok->start, len);
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Defaults                                                             */
/* ------------------------------------------------------------------ */

static void config_defaults(struct BarConfig *cfg) {
    cfg->height       = 32;
    cfg->ql_count     = 0;
    cfg->status_n     = 0;
    snprintf(cfg->font,       sizeof(cfg->font),       "sans 10");
    snprintf(cfg->icon_theme, sizeof(cfg->icon_theme), "hicolor");
    snprintf(cfg->clock_fmt,  sizeof(cfg->clock_fmt),  "%%H:%%M  %%d/%%m/%%Y");
    snprintf(cfg->power_exec, sizeof(cfg->power_exec), "");
}

/* ------------------------------------------------------------------ */
/* Parser                                                               */
/* ------------------------------------------------------------------ */

int bar_config_load(const char *path, struct BarConfig *cfg) {
    config_defaults(cfg);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_WARN("config: cannot open %s — using defaults", path);
        return 0; /* non-fatal */
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        return 0;
    }

    char *json = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (json == MAP_FAILED) {
        LOG_ERR("config: mmap failed for %s", path);
        return -1;
    }

    jsmn_parser p;
    jsmntok_t   toks[512];
    jsmn_init(&p);
    int n = jsmn_parse(&p, json, (size_t)st.st_size, toks, 512);
    if (n < 0) {
        LOG_ERR("config: JSON parse error %d in %s", n, path);
        munmap(json, (size_t)st.st_size);
        return -1;
    }

    /* Top-level must be object */
    if (n < 1 || toks[0].type != JSMN_OBJECT) {
        munmap(json, (size_t)st.st_size);
        return -1;
    }

    for (int i = 1; i < n; ) {
        if (toks[i].type != JSMN_STRING) { i++; continue; }

        if (tok_eq(json, &toks[i], "bar")) {
            /* bar sub-object */
            i++;
            if (i >= n || toks[i].type != JSMN_OBJECT) continue;
            int sub_end = i + 1 + toks[i].size * 2; /* approximate */
            i++;
            while (i < n && i < sub_end) {
                if (toks[i].type == JSMN_STRING && i+1 < n) {
                    if (tok_eq(json, &toks[i], "height")) {
                        char tmp[16]; tok_str(json, &toks[i+1], tmp, sizeof(tmp));
                        cfg->height = atoi(tmp);
                        i += 2;
                    } else if (tok_eq(json, &toks[i], "font")) {
                        tok_str(json, &toks[i+1], cfg->font, sizeof(cfg->font));
                        i += 2;
                    } else if (tok_eq(json, &toks[i], "icon_theme")) {
                        tok_str(json, &toks[i+1], cfg->icon_theme, sizeof(cfg->icon_theme));
                        i += 2;
                    } else {
                        i += 2;
                    }
                } else {
                    i++;
                }
            }
        } else if (tok_eq(json, &toks[i], "quick_launch")) {
            i++;
            if (i >= n || toks[i].type != JSMN_ARRAY) continue;
            int arr_size = toks[i].size;
            i++;
            for (int q = 0; q < arr_size && cfg->ql_count < BAR_MAX_QL; q++) {
                if (i >= n || toks[i].type != JSMN_OBJECT) { i++; continue; }
                int obj_size = toks[i].size;
                i++;
                struct BarQLButton *btn = &cfg->ql[cfg->ql_count];
                memset(btn, 0, sizeof(*btn));
                for (int k = 0; k < obj_size && i+1 < n; k++) {
                    if (tok_eq(json, &toks[i], "icon"))
                        tok_str(json, &toks[i+1], btn->icon, sizeof(btn->icon));
                    else if (tok_eq(json, &toks[i], "exec"))
                        tok_str(json, &toks[i+1], btn->exec, sizeof(btn->exec));
                    else if (tok_eq(json, &toks[i], "tooltip"))
                        tok_str(json, &toks[i+1], btn->tooltip, sizeof(btn->tooltip));
                    i += 2;
                }
                cfg->ql_count++;
            }
        } else if (tok_eq(json, &toks[i], "status")) {
            i++;
            if (i >= n || toks[i].type != JSMN_ARRAY) continue;
            int arr_size = toks[i].size;
            i++;
            for (int s = 0; s < arr_size && cfg->status_n < BAR_MAX_STATUS; s++, i++) {
                if (i >= n) break;
                tok_str(json, &toks[i], cfg->status[cfg->status_n], 32);
                cfg->status_n++;
            }
        } else if (tok_eq(json, &toks[i], "power")) {
            i++;
            if (i >= n || toks[i].type != JSMN_OBJECT) continue;
            int obj_size = toks[i].size;
            i++;
            for (int k = 0; k < obj_size && i+1 < n; k++) {
                if (tok_eq(json, &toks[i], "exec"))
                    tok_str(json, &toks[i+1], cfg->power_exec, sizeof(cfg->power_exec));
                i += 2;
            }
        } else if (tok_eq(json, &toks[i], "clock")) {
            i++;
            if (i >= n || toks[i].type != JSMN_OBJECT) continue;
            int obj_size = toks[i].size;
            i++;
            for (int k = 0; k < obj_size && i+1 < n; k++) {
                if (tok_eq(json, &toks[i], "format"))
                    tok_str(json, &toks[i+1], cfg->clock_fmt, sizeof(cfg->clock_fmt));
                i += 2;
            }
        } else {
            i++;
        }
    }

    munmap(json, (size_t)st.st_size);
    LOG_INFO("config: loaded %d quick-launch buttons, %d status modules", cfg->ql_count, cfg->status_n);
    return 0;
}
