#ifndef BAR_LOG_H
#define BAR_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>

static inline void bar_log_write(const char *level, const char *fmt, va_list ap) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char tbuf[16];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s.%03ld] [bar] [%s] ", tbuf, ts.tv_nsec / 1000000, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static inline int bar_verbose(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MAINDECK_LOG");
        v = (e && (e[0] == 'd' || e[0] == 'D')) ? 1 : 0;
    }
    return v;
}

#define LOG_INFO(...) do { \
    if (bar_verbose()) { \
        fprintf(stderr, "[bar] [INFO] " __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while(0)

#define LOG_WARN(...) do { \
    fprintf(stderr, "[bar] [WARN] " __VA_ARGS__); \
    fputc('\n', stderr); \
} while(0)

#define LOG_ERR(...) do { \
    fprintf(stderr, "[bar] [ERR ] " __VA_ARGS__); \
    fputc('\n', stderr); \
    fflush(stderr); \
} while(0)

#endif /* BAR_LOG_H */
