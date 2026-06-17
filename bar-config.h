#ifndef BAR_CONFIG_H
#define BAR_CONFIG_H

#include <stdbool.h>

#define BAR_MAX_QL     16
#define BAR_MAX_STATUS 8

struct BarQLButton {
    char  icon[128];
    char  exec[512];
    char  tooltip[128];
    int   width;          /* width multiplier: 0/1 = normal, 2 = double, etc. */
    char  bg[10];         /* optional background color: "#RRGGBB" or "#RRGGBBAA" */
    bool  has_bg;
    double bg_r, bg_g, bg_b, bg_a;
};

typedef enum {
    BAR_STATUS_UNKNOWN = 0,
    BAR_STATUS_POWER,
    BAR_STATUS_BATTERY,
    BAR_STATUS_VOLUME,
    BAR_STATUS_CLOCK
} BarStatusModule;

struct BarConfig {
    int  height;
    char font[64];
    char icon_theme[64];
    char clock_fmt[64];
    char power_exec[512];
    char volume_exec[512];

    struct BarQLButton ql[BAR_MAX_QL];
    int  ql_count;

    BarStatusModule status[BAR_MAX_STATUS];
    int  status_n;
};

BarStatusModule bar_status_from_string(const char *s);
int bar_config_load(const char *path, struct BarConfig *cfg);

#endif /* BAR_CONFIG_H */
