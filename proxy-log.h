#ifndef PROXY_LOG_H
#define PROXY_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>

static inline int proxy_verbose(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MAINDECK_LOG");
        v = (e && (e[0] == 'd' || e[0] == 'D')) ? 1 : 0;
    }
    return v;
}

static inline void vplog(int force, const char *fmt, va_list ap) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char tbuf[16];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s.%03ld] [proxy] ", tbuf, ts.tv_nsec / 1000000);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    if (force) fflush(stderr); // só erros forçam flush
}

static inline void plog(const char *fmt, ...) {
    if (!proxy_verbose()) return;
    va_list ap; va_start(ap, fmt); vplog(0, fmt, ap); va_end(ap);
}

static inline void plog_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vplog(1, fmt, ap); va_end(ap);
}

#endif /* PROXY_LOG_H */
