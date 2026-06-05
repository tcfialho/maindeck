#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pulse/pulseaudio.h>

#include "bar-state.h"
#include "bar-status.h"
#include "bar-render.h"
#include "bar-log.h"

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
/* PulseAudio async                                                     */
/* ------------------------------------------------------------------ */

static pa_mainloop     *g_pa_ml  = NULL;
static pa_context      *g_pa_ctx = NULL;
static bool             g_pa_ready = false;

static void sink_info_cb(pa_context *ctx, const pa_sink_info *info,
    int eol, void *userdata)
{
    (void)ctx; (void)userdata;
    if (eol || !info) return;

    struct BarState *bar = &g_bar;
    int vol = (int)((pa_cvolume_avg(&info->volume) * 100 + PA_VOLUME_NORM/2) / PA_VOLUME_NORM);
    if (info->mute) {
        snprintf(bar->vol_text, sizeof(bar->vol_text), "🔇");
    } else if (vol == 0) {
        snprintf(bar->vol_text, sizeof(bar->vol_text), "🔈");
    } else if (vol < 50) {
        snprintf(bar->vol_text, sizeof(bar->vol_text), "🔉 %d%%", vol);
    } else {
        snprintf(bar->vol_text, sizeof(bar->vol_text), "🔊 %d%%", vol);
    }
    bar->dirty = true;
}

static void refresh_volume(void) {
    if (!g_pa_ctx || !g_pa_ready) return;
    pa_operation *op = pa_context_get_sink_info_by_name(
        g_pa_ctx, "@DEFAULT_SINK@", sink_info_cb, NULL);
    if (op) pa_operation_unref(op);
}

static void subscribe_cb(pa_context *ctx, pa_subscription_event_type_t type,
    uint32_t idx, void *userdata)
{
    (void)ctx; (void)idx; (void)userdata;
    pa_subscription_event_type_t fac = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (fac == PA_SUBSCRIPTION_EVENT_SINK ||
        fac == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        refresh_volume();
    }
}

static void ctx_state_cb(pa_context *ctx, void *userdata) {
    (void)userdata;
    switch (pa_context_get_state(ctx)) {
    case PA_CONTEXT_READY:
        g_pa_ready = true;
        pa_context_set_subscribe_callback(ctx, subscribe_cb, NULL);
        pa_context_subscribe(ctx,
            PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT,
            NULL, NULL);
        refresh_volume();
        LOG_INFO("status: pulseaudio connected");
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        g_pa_ready = false;
        LOG_WARN("status: pulseaudio disconnected");
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int bar_status_init(void) {
    /* Clock and battery initial values */
    update_clock();
    update_battery();

    /* PulseAudio mainloop (non-threaded, polled manually) */
    g_pa_ml = pa_mainloop_new();
    if (!g_pa_ml) {
        LOG_WARN("status: pa_mainloop_new failed — no volume display");
        return -1;
    }

    g_pa_ctx = pa_context_new(pa_mainloop_get_api(g_pa_ml), "maindeck-bar");
    if (!g_pa_ctx) {
        LOG_WARN("status: pa_context_new failed");
        pa_mainloop_free(g_pa_ml);
        g_pa_ml = NULL;
        return -1;
    }

    pa_context_set_state_callback(g_pa_ctx, ctx_state_cb, NULL);
    if (pa_context_connect(g_pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        LOG_WARN("status: pa_context_connect failed");
        pa_context_unref(g_pa_ctx);
        pa_mainloop_free(g_pa_ml);
        g_pa_ctx = NULL;
        g_pa_ml  = NULL;
        return -1;
    }

    /* PA mainloop doesn't expose a single pollable fd — caller must call
     * bar_status_pulse_dispatch() regularly (e.g. after each poll timeout). */
    return -1;
}

void bar_status_pulse_dispatch(void) {
    if (!g_pa_ml) return;
    int ret = 0;
    pa_mainloop_iterate(g_pa_ml, 0, &ret);
}

void bar_status_tick(void) {
    update_clock();
    update_battery();
    g_bar.dirty = true;
    /* Also dispatch any pending PA events */
    bar_status_pulse_dispatch();
}

void bar_status_cleanup(void) {
    if (g_pa_ctx) {
        pa_context_disconnect(g_pa_ctx);
        pa_context_unref(g_pa_ctx);
        g_pa_ctx = NULL;
    }
    if (g_pa_ml) {
        pa_mainloop_free(g_pa_ml);
        g_pa_ml = NULL;
    }
}
