#include <stdlib.h>
#include <unistd.h>

#include "bar-state.h"
#include "bar-quicklaunch.h"
#include "bar-log.h"

void bar_quicklaunch_exec(int idx) {
    struct BarState *bar = &g_bar;
    if (idx < 0 || idx >= bar->config.ql_count) return;

    const char *cmd = bar->config.ql[idx].exec;
    if (!cmd || !cmd[0]) return;

    LOG_INFO("quicklaunch: exec '%s'", cmd);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    /* parent continues; child is detached via setsid */
}
