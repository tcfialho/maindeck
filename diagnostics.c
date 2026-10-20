#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "diagnostics.h"

static int parse_level(const char *s) {
    if (!s) return 1;
    if (!strncmp(s,"trace",5)) return 4;
    if (!strncmp(s,"debug",5)) return 3;
    if (!strncmp(s,"info",4)) return 2;
    return 1;
}
int md_diagnostic_level(void) {
    static time_t checked = -1;
    static int level = 1, refreshing;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    if (__atomic_load_n(&checked,__ATOMIC_ACQUIRE) == now.tv_sec)
        return __atomic_load_n(&level,__ATOMIC_ACQUIRE);
    // Never wait for diagnostic configuration in a UI or loader thread.
    if (__atomic_exchange_n(&refreshing,1,__ATOMIC_ACQUIRE))
        return __atomic_load_n(&level,__ATOMIC_ACQUIRE);
    int next = parse_level(getenv("MAINDECK_LOG"));
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && dir[0]) {
        char path[256],buf[16];
        if (snprintf(path,sizeof(path),"%s/maindeck-log-level",dir) < (int)sizeof(path)) {
            FILE *f = fopen(path,"r");
            if (f) { if (fgets(buf,sizeof(buf),f)) next = parse_level(buf); fclose(f); }
        }
    }
    __atomic_store_n(&level,next,__ATOMIC_RELEASE);
    __atomic_store_n(&checked,now.tv_sec,__ATOMIC_RELEASE);
    __atomic_store_n(&refreshing,0,__ATOMIC_RELEASE);
    return next;
}
