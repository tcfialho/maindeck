#ifndef BAR_CONFIG_H
#define BAR_CONFIG_H

#define BAR_MAX_QL     16
#define BAR_MAX_STATUS 8

struct BarQLButton {
    char icon[128];
    char exec[512];
    char tooltip[128];
};

struct BarConfig {
    int  height;
    char font[64];
    char icon_theme[64];
    char clock_fmt[64];
    char power_exec[512];

    struct BarQLButton ql[BAR_MAX_QL];
    int  ql_count;

    char status[BAR_MAX_STATUS][32];
    int  status_n;
};

int bar_config_load(const char *path, struct BarConfig *cfg);

#endif /* BAR_CONFIG_H */
