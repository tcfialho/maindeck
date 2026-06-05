#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bar-state.h"
#include "bar-status.h"

/* ------------------------------------------------------------------ */
/* Clock + battery                                                      */
/* ------------------------------------------------------------------ */

static void update_clock(void) {
    struct BarState *bar = &g_bar;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(bar->clock_text, sizeof(bar->clock_text), bar->config.clock_fmt, &tm);
}

static void update_battery(void) {
    struct BarState *bar = &g_bar;
    bar->bat_level    = -1;
    bar->bat_charging = false;

    static const char *bat_paths[] = {
        "/sys/class/power_supply/BAT0",
        "/sys/class/power_supply/BAT1",
        NULL,
    };

    for (int i = 0; bat_paths[i]; i++) {
        char path[128];
        snprintf(path, sizeof(path), "%s/capacity", bat_paths[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        int cap = -1;
        fscanf(f, "%d", &cap);
        fclose(f);
        if (cap < 0) continue;

        char status[32] = "Unknown";
        snprintf(path, sizeof(path), "%s/status", bat_paths[i]);
        f = fopen(path, "r");
        if (f) {
            if (!fgets(status, sizeof(status), f)) status[0] = '\0';
            fclose(f);
            char *nl = strchr(status, '\n');
            if (nl) *nl = '\0';
        }

        bar->bat_level    = cap;
        bar->bat_charging = (strncmp(status, "Charging", 8) == 0 ||
                             strncmp(status, "Full", 4) == 0);
        snprintf(bar->bat_text, sizeof(bar->bat_text), "%d%%", cap);
        return;
    }

    bar->bat_text[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int bar_status_init(void) {
    update_clock();
    update_battery();
    /* Static volume icon — clicking opens pavucontrol */
    snprintf(g_bar.vol_text, sizeof(g_bar.vol_text), "vol");
    return -1;
}

void bar_status_tick(void) {
    update_clock();
    update_battery();
    g_bar.dirty = true;
}

void bar_status_cleanup(void) {
    /* nothing */
}
