# Review Input: evitar_duplicacao_barra

## Cabeçalho de contexto
- Objetivo: Evitar a execução de instâncias duplicadas da barra `maindeck-bar` ao reiniciar sessões no River.
- Critérios de aceitação: Uso de lockfile no `XDG_RUNTIME_DIR` via `flock`. Envio de `SIGTERM` para a instância anterior se esta for validada como `maindeck-bar` (via `/proc/[pid]/comm`). Timeout máximo de 2 segundos para liberar o lock de forma limpa.
- Gate: `ninja -C build` compilou com sucesso e sem qualquer aviso em `bar-main.c`.

## Plano
```markdown
1. Decisões do Judge:
   - flock(LOCK_EX|LOCK_NB) para validação atômica da instância ativa.
   - SIGTERM para o PID listado após checagem de kill(pid, 0) e comm (/proc/[pid]/comm).
   - Loop com nanosleep(50ms) por até 40 iterações (2s).
   - singleton_acquire() chamado na primeira linha útil de main(), mantendo o fd aberto.
2. Arquivos a tocar: bar-main.c
3. Implementação da validação e aquisição de lock.
4. Uso de strerror(errno) para garantir C99 estrito e evitar avisos de compiler.
```

## Diff
```diff
diff --git a/bar-main.c b/bar-main.c
index 2e87a9d..399e694 100644
--- a/bar-main.c
+++ b/bar-main.c
@@ -11,6 +11,8 @@
 #include <sys/socket.h>
 #include <sys/un.h>
 #include <time.h>
+#include <sys/file.h>
+#include <fcntl.h>
 
 #include <wayland-client.h>
 
@@ -151,7 +153,7 @@ static void ipc_init(void) {
 
     bar->ipc_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
     if (bar->ipc_sock < 0) {
-        LOG_WARN("main: IPC socket creation failed: %m");
+        LOG_WARN("main: IPC socket creation failed: %s", strerror(errno));
     }
 
     /* Notify socket: WM sends fullscreen_on/fullscreen_off here */
@@ -167,7 +169,7 @@ static void ipc_init(void) {
                 bar->notify_sock = fd;
                 LOG_INFO("main: notify socket bound at %s", notify_path);
             } else {
-                LOG_WARN("main: notify socket bind failed: %m");
+                LOG_WARN("main: notify socket bind failed: %s", strerror(errno));
                 close(fd);
                 bar->notify_sock = -1;
             }
@@ -216,10 +218,100 @@ static int ms_until_next_minute(void) {
     return ms_left;
 }
 
+/* ------------------------------------------------------------------ */
+/* Singleton lock                                                       */
+/* ------------------------------------------------------------------ */
+
+static int comm_matches(pid_t pid) {
+    char path[64];
+    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
+    int fd = open(path, O_RDONLY | O_CLOEXEC);
+    if (fd < 0) {
+        return 0;
+    }
+    char buf[32];
+    ssize_t n = read(fd, buf, sizeof(buf) - 1);
+    close(fd);
+    if (n <= 0) {
+        return 0;
+    }
+    buf[n] = '\0';
+    size_t len = strcspn(buf, "\n");
+    buf[len] = '\0';
+    return strcmp(buf, "maindeck-bar") == 0;
+}
+
+static int singleton_acquire(void) {
+    const char *dir = getenv("XDG_RUNTIME_DIR");
+    if (!dir) {
+        LOG_WARN("singleton: XDG_RUNTIME_DIR not set, skipping singleton check");
+        return -1;
+    }
+    char path[256];
+    if ((size_t)snprintf(path, sizeof(path), "%s/maindeck-bar.lock", dir) >= sizeof(path)) {
+        LOG_WARN("singleton: path too long");
+        return -1;
+    }
+
+    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
+    if (fd < 0) {
+        LOG_WARN("singleton: failed to open lockfile %s: %s", path, strerror(errno));
+        return -1;
+    }
+
+    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
+        goto own;
+    }
+
+    if (errno == EWOULDBLOCK) {
+        char buf[32];
+        ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
+        if (n > 0) {
+            buf[n] = '\0';
+            long pid_long = strtol(buf, NULL, 10);
+            if (pid_long > 0) {
+                pid_t old_pid = (pid_t)pid_long;
+                if (kill(old_pid, 0) == 0 && comm_matches(old_pid)) {
+                    LOG_INFO("singleton: sending SIGTERM to existing instance (PID %d)", (int)old_pid);
+                    kill(old_pid, SIGTERM);
+                }
+            }
+        }
+
+        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
+        for (int i = 0; i < 40; i++) {
+            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
+                goto own;
+            }
+            nanosleep(&ts, NULL);
+        }
+
+        LOG_WARN("singleton: timeout waiting for lock, proceeding anyway");
+        goto own;
+    }
+
+    LOG_WARN("singleton: flock failed: %s", strerror(errno));
+    close(fd);
+    return -1;
+
+own:
+    if (ftruncate(fd, 0) == 0) {
+        char buf[32];
+        int n = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
+        if (n > 0) {
+            (void)pwrite(fd, buf, n, 0);
+        }
+    } else {
+        LOG_WARN("singleton: ftruncate failed: %s", strerror(errno));
+    }
+    return fd;
+}
+
 /* ------------------------------------------------------------------ */
 /* main                                                                 */
 /* ------------------------------------------------------------------ */
 
+
 int main(void) {
 #ifdef __GLIBC__
     mallopt(M_ARENA_MAX, 1);
@@ -234,6 +326,10 @@ int main(void) {
     signal(SIGTERM, on_signal);
     signal(SIGCHLD, SIG_IGN);  /* reap children automatically */
 
+    /* Acquire singleton lock to prevent multiple concurrent instances */
+    int lock_fd = singleton_acquire();
+    (void)lock_fd; /* keep fd open until process exits to hold the lock */
+
     memset(&g_bar, 0, sizeof(g_bar));
     g_bar.shm_fd     = -1;
     g_bar.ipc_sock   = -1;
```
