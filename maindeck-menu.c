#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include <sys/resource.h>
#include <malloc.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <ctype.h>
#include <time.h>

#include <wayland-client.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "bar-icons.h"

#define MENU_WIDTH 320
#define MENU_HEIGHT 480
#define ITEM_HEIGHT 34
#define MAX_ITEMS 12
#define SOCKET_NAME "maindeck-menu.sock"

#define LOG_INFO(...) do { fprintf(stdout, "[menu:info] "); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)
#define LOG_ERR(...) do { fprintf(stderr, "[menu:err] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)

// Estutura de aplicativo (.desktop)
typedef struct {
    char *id;       // basename do arquivo .desktop
    char *name;     // Nome visível
    char *exec;     // Comando a executar
    char *icon;     // Nome/caminho do ícone
    bool terminal;  // Executar no terminal?
} App;

// Estrutura de estado global do app
typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;

    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_surface *ptr_surface;

    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_table *xkb_compose_table;
    struct xkb_compose_state *xkb_compose_state;

    // Superfície MENU
    struct wl_surface *menu_surf;
    struct zwlr_layer_surface_v1 *menu_layer;
    int shm_fd;
    void *shm_data;
    size_t buf_size;
    struct wl_shm_pool *shm_pool;
    struct wl_buffer *buffers[2];
    bool buffer_busy[2];
    int cur_buf;
    int menu_w, menu_h;

    // Superfície CAPTURA (background)
    struct wl_surface *bg_surf;
    struct zwlr_layer_surface_v1 *bg_layer;
    struct wp_viewport *bg_viewport;
    struct wl_buffer *bg_buf_1x1;
    int bg_shm_fd;
    struct wl_shm_pool *bg_shm_pool;
    bool bg_created;
    int out_w, out_h;

    // IPC e Fallback de Foco
    int sock_fd;
    char sock_path[108];
    int focus_timer_fd;

    // Estado da UI
    char query[256];
    size_t query_len;
    int scroll_offset;
    App *apps;
    size_t napps;
    int *filtered;
    int *scores;
    size_t nfiltered;
    int sel;
    enum { ZONE_SEARCH, ZONE_LIST } focus_zone;
    double ptr_x;
    double ptr_y;
    bool running;
    bool dirty;
    bool configured;
    bool menu_has_kbd_focus;
} AppState;

static AppState g_app;

// --- Helper: Shared memory allocation ---
static int create_shm_file(size_t size) {
    char name[64];
    snprintf(name, sizeof(name), "/maindeck-menu-%d-%d", (int)getpid(), (int)rand());
    int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fd = memfd_create("maindeck-menu", MFD_CLOEXEC);
        if (fd < 0) return -1;
    } else {
        shm_unlink(name);
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// --- Parser de arquivos .desktop ---
static char *trim_and_dup(const char *str) {
    while (isspace((unsigned char)*str)) str++;
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) len--;
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len);
        dup[len] = '\0';
    }
    return dup;
}

static char *clean_exec(const char *exec_line) {
    // Remove códigos de campo do desktop (%f, %F, %u, %U, %i, %c, %k, %v, %m, %%)
    char *clean = malloc(strlen(exec_line) + 1);
    if (!clean) return NULL;
    char *dst = clean;
    const char *src = exec_line;
    while (*src) {
        if (*src == '%') {
            src++;
            if (*src == '%') {
                *dst++ = '%';
                src++;
            } else if (*src && strchr("fFuUiCckvm", *src)) {
                src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    char *res = trim_and_dup(clean);
    free(clean);
    return res;
}

static void load_desktop_file(const char *filepath, const char *filename) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    char *name = NULL;
    char *exec = NULL;
    char *icon = NULL;
    bool terminal = false;
    bool no_display = false;
    bool hidden = false;
    char *type = NULL;
    char *try_exec = NULL;

    char line[1024];
    bool in_entry = false;

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = line;
        while (isspace((unsigned char)*trimmed)) trimmed++;
        if (trimmed[0] == '#' || trimmed[0] == '\0') continue;

        if (trimmed[0] == '[') {
            if (strncmp(trimmed, "[Desktop Entry]", 15) == 0) {
                in_entry = true;
            } else {
                // Outro grupo iniciado (Desktop Action, etc.) -> pare o parse
                break;
            }
            continue;
        }

        if (!in_entry) continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_and_dup(trimmed);
        char *value = trim_and_dup(eq + 1);
        *eq = '='; // restaura

        if (strcmp(key, "Type") == 0) type = value;
        else if (strcmp(key, "Name") == 0 && !name) name = value; // pega o Name padrão primeiro
        else if (strcmp(key, "Exec") == 0) exec = value;
        else if (strcmp(key, "Icon") == 0) icon = value;
        else if (strcmp(key, "TryExec") == 0) try_exec = value;
        else if (strcmp(key, "Terminal") == 0) {
            terminal = (strcmp(value, "true") == 0);
            free(value);
        } else if (strcmp(key, "NoDisplay") == 0) {
            no_display = (strcmp(value, "true") == 0);
            free(value);
        } else if (strcmp(key, "Hidden") == 0) {
            hidden = (strcmp(value, "true") == 0);
            free(value);
        } else {
            free(value);
        }
        free(key);
    }
    fclose(f);

    bool ok = true;
    if (!type || strcmp(type, "Application") != 0) ok = false;
    if (no_display || hidden) ok = false;
    if (!name || !exec) ok = false;

    if (ok && try_exec) {
        char *bin = try_exec;
        if (bin[0] != '/') {
            // Verifica no PATH
            char *path = getenv("PATH");
            if (path) {
                char *path_dup = strdup(path);
                char *dir = strtok(path_dup, ":");
                bool found = false;
                while (dir) {
                    char check_path[1024];
                    snprintf(check_path, sizeof(check_path), "%s/%s", dir, bin);
                    if (access(check_path, X_OK) == 0) {
                        found = true;
                        break;
                    }
                    dir = strtok(NULL, ":");
                }
                free(path_dup);
                if (!found) ok = false;
            } else {
                ok = false;
            }
        } else {
            if (access(bin, X_OK) != 0) ok = false;
        }
    }

    if (ok) {
        // Verifica se já temos este ID cadastrado (deduplicado por basename)
        bool dup = false;
        for (size_t i = 0; i < g_app.napps; i++) {
            if (strcmp(g_app.apps[i].id, filename) == 0) {
                dup = true;
                break;
            }
        }

        if (!dup) {
            App *tmp = realloc(g_app.apps, (g_app.napps + 1) * sizeof(App));
            if (tmp) {
                g_app.apps = tmp;
                App *app = &g_app.apps[g_app.napps];
                app->id = strdup(filename);
                app->name = name;
                app->exec = clean_exec(exec);
                app->icon = icon;
                app->terminal = terminal;
                g_app.napps++;
                name = NULL; // consumido
                icon = NULL; // consumido
            }
        }
    }

    free(type);
    free(name);
    free(exec);
    free(icon);
    free(try_exec);
}

static void scan_desktop_dir(const char *dirpath) {
    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t len = strlen(de->d_name);
        if (len > 8 && strcmp(de->d_name + len - 8, ".desktop") == 0) {
            char filepath[1024];
            snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, de->d_name);
            load_desktop_file(filepath, de->d_name);
        }
    }
    closedir(d);
}

static void load_all_apps(void) {
    // 1. $XDG_DATA_HOME/applications (default ~/.local/share/applications)
    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    char path[1024];
    if (data_home) {
        snprintf(path, sizeof(path), "%s/applications", data_home);
        scan_desktop_dir(path);
    } else if (home) {
        snprintf(path, sizeof(path), "%s/.local/share/applications", home);
        scan_desktop_dir(path);
    }

    // 2. $XDG_DATA_DIRS/applications (default /usr/share/applications:/usr/local/share/applications)
    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs) {
        data_dirs = "/usr/share/applications:/usr/local/share/applications";
    }
    char *dirs_dup = strdup(data_dirs);
    char *dir = strtok(dirs_dup, ":");
    while (dir) {
        snprintf(path, sizeof(path), "%s/applications", dir);
        scan_desktop_dir(path);
        dir = strtok(NULL, ":");
    }
    free(dirs_dup);
}

// --- Busca Fuzzy ---
static int compare_apps_alphabetical(const void *a, const void *b) {
    int idx_a = *(const int *)a;
    int idx_b = *(const int *)b;
    return strcasecmp(g_app.apps[idx_a].name, g_app.apps[idx_b].name);
}

static int compare_apps_fuzzy(const void *a, const void *b) {
    int idx_a = *(const int *)a;
    int idx_b = *(const int *)b;
    int score_a = g_app.scores[idx_a];
    int score_b = g_app.scores[idx_b];
    if (score_a != score_b) {
        return score_b - score_a; // decrescente
    }
    return strcasecmp(g_app.apps[idx_a].name, g_app.apps[idx_b].name);
}

static bool fuzzy_match(const char *str, const char *query, int *out_score) {
    int score = 0;
    const char *s = str;
    const char *q = query;

    int last_match_idx = -2;
    int consecutive = 0;

    while (*q) {
        char target = tolower((unsigned char)*q);
        const char *match = NULL;
        const char *curr = s;
        while (*curr) {
            if (tolower((unsigned char)*curr) == target) {
                match = curr;
                break;
            }
            curr++;
        }
        if (!match) return false;

        int idx = (int)(match - str);
        score += 10; // base points

        // Bônus contíguo
        if (idx == last_match_idx + 1) {
            consecutive++;
            score += 5 * consecutive;
        } else {
            consecutive = 0;
        }

        // Bônus de início de palavra
        if (idx == 0 || str[idx - 1] == ' ' || str[idx - 1] == '-' || str[idx - 1] == '_') {
            score += 15;
        }

        last_match_idx = idx;
        s = match + 1;
        q++;
    }

    *out_score = score;
    return true;
}

static void filter_apps(void) {
    g_app.nfiltered = 0;
    if (g_app.query_len == 0) {
        for (size_t i = 0; i < g_app.napps; i++) {
            g_app.filtered[g_app.nfiltered++] = (int)i;
        }
        qsort(g_app.filtered, g_app.nfiltered, sizeof(int), compare_apps_alphabetical);
    } else {
        for (size_t i = 0; i < g_app.napps; i++) {
            int score = 0;
            if (fuzzy_match(g_app.apps[i].name, g_app.query, &score)) {
                g_app.scores[i] = score;
                g_app.filtered[g_app.nfiltered++] = (int)i;
            } else if (g_app.apps[i].exec && fuzzy_match(g_app.apps[i].exec, g_app.query, &score)) {
                g_app.scores[i] = score / 2; // peso menor
                g_app.filtered[g_app.nfiltered++] = (int)i;
            } else {
                g_app.scores[i] = 0;
            }
        }
        qsort(g_app.filtered, g_app.nfiltered, sizeof(int), compare_apps_fuzzy);
    }

    if (g_app.sel >= (int)g_app.nfiltered) {
        g_app.sel = g_app.nfiltered > 0 ? (int)g_app.nfiltered - 1 : 0;
    }
    if (g_app.sel < 0) g_app.sel = 0;
}

// --- Spawn de Aplicativo ---
static void spawn_app(App *app) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        // fecha fds herdados por segurança
        for (int i = 3; i < 256; i++) close(i);
        // redireciona 0/1/2
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            close(devnull);
        }

        if (app->terminal) {
            const char *term = getenv("TERMINAL");
            if (!term) term = "kitty"; // fallback
            char *cmd;
            if (asprintf(&cmd, "%s -e %s", term, app->exec) >= 0) {
                execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
                free(cmd);
            }
        } else {
            execl("/bin/sh", "sh", "-c", app->exec, (char *)NULL);
        }
        _exit(127);
    }
}

// --- IPC: Instância única e Toggle ---
static bool setup_ipc(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) {
        LOG_ERR("XDG_RUNTIME_DIR not set");
        return false;
    }
    snprintf(g_app.sock_path, sizeof(g_app.sock_path), "%s/%s", dir, SOCKET_NAME);

    g_app.sock_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (g_app.sock_fd < 0) {
        LOG_ERR("IPC socket failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, g_app.sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_app.sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        LOG_INFO("IPC: Escutando como instância primária");
        return true;
    }

    if (errno == EADDRINUSE) {
        // Outro menu já aberto -> tenta enviar comando de fechamento
        int temp_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        bool sent = false;
        if (temp_fd >= 0) {
            if (sendto(temp_fd, "C", 1, 0, (struct sockaddr *)&addr, sizeof(addr)) >= 0) {
                sent = true;
            }
            close(temp_fd);
        }
        if (sent) {
            close(g_app.sock_fd);
            LOG_INFO("IPC: Enviado sinal CLOSE para instância ativa. Encerrando.");
            exit(0); // toggle close bem-sucedido
        }
    }

    // Socket órfão (bind falhou mas ninguém ouve nele)
    unlink(g_app.sock_path);
    if (bind(g_app.sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        LOG_INFO("IPC: Escutando como instância primária após limpar socket órfão");
        return true;
    }

    close(g_app.sock_fd);
    return false;
}

static void cleanup_ipc(void) {
    if (g_app.sock_fd >= 0) {
        close(g_app.sock_fd);
        unlink(g_app.sock_path);
        g_app.sock_fd = -1;
    }
}

// --- Criação das superfícies e buffers Wayland ---
static void buffer_release(void *data, struct wl_buffer *buf) {
    AppState *app = &g_app;
    if (buf == app->buffers[0]) app->buffer_busy[0] = false;
    else if (buf == app->buffers[1]) app->buffer_busy[1] = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void create_menu_buffers(int w, int h) {
    AppState *app = &g_app;
    int stride = w * 4;
    size_t size = (size_t)stride * h;

    app->shm_fd = create_shm_file(size * 2);
    if (app->shm_fd < 0) {
        LOG_ERR("shm_file allocation failed");
        exit(1);
    }

    app->shm_data = mmap(NULL, size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, app->shm_fd, 0);
    if (app->shm_data == MAP_FAILED) {
        LOG_ERR("mmap failed");
        close(app->shm_fd);
        exit(1);
    }

    app->buf_size = size;
    app->shm_pool = wl_shm_create_pool(app->shm, app->shm_fd, (int)(size * 2));

    app->buffers[0] = wl_shm_pool_create_buffer(app->shm_pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    app->buffers[1] = wl_shm_pool_create_buffer(app->shm_pool, (int)size, w, h, stride, WL_SHM_FORMAT_ARGB8888);

    wl_buffer_add_listener(app->buffers[0], &buffer_listener, NULL);
    wl_buffer_add_listener(app->buffers[1], &buffer_listener, NULL);
}

static void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, -M_PI_2);
    cairo_close_path(cr);
}

static cairo_surface_t *s_cs[2]   = { NULL, NULL };
static cairo_t         *s_cr[2]   = { NULL, NULL };
static PangoLayout     *s_lay[2]  = { NULL, NULL };
static PangoFontDescription *s_font_desc = NULL;
static int s_cs_w = 0, s_cs_h = 0;

static void ensure_cairo_menu(AppState *app, unsigned char *data) {
    int b = app->cur_buf;
    if (s_cs[b] && s_cs_w == app->menu_w && s_cs_h == app->menu_h)
        return;

    if (s_cs_w != app->menu_w || s_cs_h != app->menu_h) {
        for (int i = 0; i < 2; i++) {
            if (s_lay[i]) { g_object_unref(s_lay[i]); s_lay[i] = NULL; }
            if (s_cr[i])  { cairo_destroy(s_cr[i]); s_cr[i] = NULL; }
            if (s_cs[i])  { cairo_surface_destroy(s_cs[i]); s_cs[i] = NULL; }
        }
    }

    s_cs[b] = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32,
        app->menu_w, app->menu_h, app->menu_w * 4);
    s_cr[b]  = cairo_create(s_cs[b]);

    s_lay[b] = pango_cairo_create_layout(s_cr[b]);
    pango_layout_set_width(s_lay[b], (app->menu_w - 40) * PANGO_SCALE);
    pango_layout_set_ellipsize(s_lay[b], PANGO_ELLIPSIZE_END);

    if (!s_font_desc) s_font_desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(s_lay[b], s_font_desc);

    s_cs_w = app->menu_w; s_cs_h = app->menu_h;
}

static void render(void) {
    AppState *app = &g_app;
    if (!app->configured) return;

    // Seleciona buffer livre
    int b = app->cur_buf;
    if (app->buffer_busy[b]) {
        b = (b == 0) ? 1 : 0;
        if (app->buffer_busy[b]) {
            // Ambos ocupados? Drena eventos para liberar
            wl_display_roundtrip(app->display);
            if (app->buffer_busy[b]) {
                app->dirty = true;
                return;
            }
        }
    }
    app->cur_buf = b;
    app->buffer_busy[b] = true;

    // Ajusta scroll_offset com base na seleção
    if (app->sel >= app->scroll_offset + MAX_ITEMS) {
        app->scroll_offset = app->sel - MAX_ITEMS + 1;
    } else if (app->sel < app->scroll_offset) {
        app->scroll_offset = app->sel;
    }
    if (app->scroll_offset < 0) app->scroll_offset = 0;

    unsigned char *data = (unsigned char *)app->shm_data + b * app->buf_size;
    memset(data, 0, app->buf_size);

    ensure_cairo_menu(app, data);
    cairo_t *cr = s_cr[b];
    PangoLayout *layout = s_lay[b];

    cairo_save(cr);

    // 1. Fundo do Menu (sólido combinando com a barra #1C1C24)
    draw_rounded_rect(cr, 1, 1, app->menu_w - 2, app->menu_h - 2, 10);
    cairo_set_source_rgba(cr, 0.11, 0.11, 0.14, 1.0);
    cairo_fill_preserve(cr);

    // Borda fina (sólida #38384D)
    cairo_set_source_rgba(cr, 0.22, 0.22, 0.30, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // 2. Caixa de busca (sólida #11111b)
    draw_rounded_rect(cr, 12, 12, app->menu_w - 24, 34, 6);
    cairo_set_source_rgba(cr, 0.067, 0.067, 0.106, 1.0);
    cairo_fill_preserve(cr);
    if (app->focus_zone == ZONE_SEARCH) {
        cairo_set_source_rgba(cr, 0.18, 0.36, 0.70, 1.0); // borda ativa azul sólida #2e5cb3
    } else {
        cairo_set_source_rgba(cr, 0.22, 0.22, 0.30, 1.0);
    }
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    if (app->query_len > 0) {
        pango_layout_set_text(layout, app->query, -1);
        cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, 1.0); // COL_TEXT
    } else {
        pango_layout_set_text(layout, "Pesquisar aplicativo...", -1);
        cairo_set_source_rgba(cr, 0.55, 0.55, 0.60, 1.0); // COL_TEXT_DIM
    }
    cairo_move_to(cr, 20, 20);
    pango_cairo_show_layout(cr, layout);

    // Cursor pulsante (barra estática quando focado)
    if (app->focus_zone == ZONE_SEARCH) {
        int w_text, h_text;
        pango_layout_get_pixel_size(layout, &w_text, &h_text);
        cairo_set_source_rgba(cr, 0.18, 0.36, 0.70, 1.0); // Cursor azul
        cairo_rectangle(cr, 20 + w_text + 1, 20, 2, 16);
        cairo_fill(cr);
    }

    // 3. Desenha a lista de aplicativos filtrados (com scroll)
    int list_y = 56;
    for (int i = 0; i < MAX_ITEMS; i++) {
        int idx = app->scroll_offset + i;
        if (idx >= (int)app->nfiltered) break;
        int app_idx = app->filtered[idx];
        App *a = &app->apps[app_idx];

        bool is_selected = (app->focus_zone == ZONE_LIST && idx == app->sel);

        // Retângulo de hover do item selecionado (sólido #38384D)
        if (is_selected) {
            draw_rounded_rect(cr, 12, list_y, app->menu_w - 24, ITEM_HEIGHT, 5);
            cairo_set_source_rgba(cr, 0.22, 0.22, 0.30, 1.0);
            cairo_fill(cr);
        }

        // Carrega e desenha o ícone do aplicativo
        cairo_surface_t *icon_surf = NULL;
        if (a->icon) {
            icon_surf = bar_icon_get(a->icon, 18);
        }
        bar_icon_draw(cr, icon_surf, 20, list_y + 8, 18);

        pango_layout_set_text(layout, a->name, -1);
        cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, 1.0); // COL_TEXT (mesma cor branca da barra)
        cairo_move_to(cr, 46, list_y + 7);
        pango_cairo_show_layout(cr, layout);

        list_y += ITEM_HEIGHT;
    }

    cairo_restore(cr);

    wl_surface_attach(app->menu_surf, app->buffers[b], 0, 0);
    wl_surface_damage_buffer(app->menu_surf, 0, 0, app->menu_w, app->menu_h);
    wl_surface_commit(app->menu_surf);
}

#if 0
// --- Criação da Superfície de Captura (bg_surf) ---
static void bg_layer_configure(void *data, struct zwlr_layer_surface_v1 *surf, uint32_t serial, uint32_t w, uint32_t h) {
    (void)data;
    AppState *app = &g_app;
    zwlr_layer_surface_v1_ack_configure(surf, serial);

    int width = (int)w;
    int height = (int)h;
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;

    app->out_w = width;
    app->out_h = height;

    if (app->viewporter && !app->bg_viewport) {
        app->bg_viewport = wp_viewporter_get_viewport(app->viewporter, app->bg_surf);
    }
    if (app->bg_viewport) {
        wp_viewport_set_destination(app->bg_viewport, width, height);
    }

    if (app->bg_buf_1x1) {
        wl_surface_attach(app->bg_surf, app->bg_buf_1x1, 0, 0);
    }
    wl_surface_set_opaque_region(app->bg_surf, NULL);

    struct wl_region *region = wl_compositor_create_region(app->compositor);
    if (region) {
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_input_region(app->bg_surf, region);
        wl_region_destroy(region);
    }
    wl_surface_damage_buffer(app->bg_surf, 0, 0, 1, 1);
    wl_surface_commit(app->bg_surf);
}

static void bg_layer_closed(void *data, struct zwlr_layer_surface_v1 *surf) {
    (void)data; (void)surf;
    LOG_INFO("captura: background layer closed");
}

static const struct zwlr_layer_surface_v1_listener bg_layer_listener = {
    .configure = bg_layer_configure,
    .closed = bg_layer_closed,
};

static void create_capture_surface(void) {
    return; // Deativado para teste de dismiss nativo via ON_DEMAND
    AppState *app = &g_app;
    if (app->bg_created) return;

    app->bg_surf = wl_compositor_create_surface(app->compositor);
    if (!app->bg_surf) return;

    app->bg_layer = zwlr_layer_shell_v1_get_layer_surface(app->layer_shell, app->bg_surf, NULL,
                        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "maindeck-menu-bg");
    if (!app->bg_layer) {
        wl_surface_destroy(app->bg_surf);
        app->bg_surf = NULL;
        return;
    }

    zwlr_layer_surface_v1_set_anchor(app->bg_layer,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(app->bg_layer, -1);
    zwlr_layer_surface_v1_set_size(app->bg_layer, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(app->bg_layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(app->bg_layer, &bg_layer_listener, NULL);

    // Aloca buffer 1x1 estático
    int fd = create_shm_file(4);
    if (fd >= 0) {
        void *data = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data != MAP_FAILED) {
            memset(data, 0, 4);
            app->bg_shm_pool = wl_shm_create_pool(app->shm, fd, 4);
            app->bg_buf_1x1 = wl_shm_pool_create_buffer(app->bg_shm_pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
            app->bg_shm_fd = fd;
        } else {
            close(fd);
        }
    }

    wl_surface_commit(app->bg_surf);
    app->bg_created = true;
    LOG_INFO("captura: superfície invisível de background criada");
}

static void destroy_capture_surface(void) {
    AppState *app = &g_app;
    if (!app->bg_created) return;

    if (app->bg_viewport) {
        wp_viewport_destroy(app->bg_viewport);
        app->bg_viewport = NULL;
    }
    if (app->bg_buf_1x1) {
        wl_buffer_destroy(app->bg_buf_1x1);
        app->bg_buf_1x1 = NULL;
    }
    if (app->bg_shm_pool) {
        wl_shm_pool_destroy(app->bg_shm_pool);
        app->bg_shm_pool = NULL;
    }
    if (app->bg_shm_fd >= 0) {
        close(app->bg_shm_fd);
        app->bg_shm_fd = -1;
    }
    if (app->bg_layer) {
        zwlr_layer_surface_v1_destroy(app->bg_layer);
        app->bg_layer = NULL;
    }
    if (app->bg_surf) {
        wl_surface_destroy(app->bg_surf);
        app->bg_surf = NULL;
    }
    app->bg_created = false;
}
#endif

// --- Callbacks de Teclado (XKB / input) ---
static void keyboard_keymap(void *data, struct wl_keyboard *kbd, uint32_t format, int fd, uint32_t size) {
    AppState *app = &g_app;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    if (app->xkb_keymap) xkb_keymap_unref(app->xkb_keymap);
    if (app->xkb_state) xkb_state_unref(app->xkb_state);

    app->xkb_keymap = xkb_keymap_new_from_string(app->xkb_context, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    close(fd);

    if (!app->xkb_keymap) {
        LOG_ERR("xkb keymap compilation failed");
        return;
    }
    app->xkb_state = xkb_state_new(app->xkb_keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *surf, struct wl_array *keys) {
    (void)data; (void)kbd; (void)serial; (void)keys;
    AppState *app = &g_app;
    if (surf == app->menu_surf) {
        app->menu_has_kbd_focus = true;
        LOG_INFO("Teclado: menu focado");
    }
}

static void keyboard_leave(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *surf) {
    (void)data; (void)kbd; (void)serial;
    AppState *app = &g_app;
    if (surf == app->menu_surf && app->menu_has_kbd_focus) {
        LOG_INFO("Teclado: foco perdido. Fechando.");
        app->running = false;
    }
}

static void keyboard_key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time, uint32_t key, uint32_t state_val) {
    (void)data; (void)kbd; (void)serial; (void)time;
    AppState *app = &g_app;
    if (!app->xkb_state) return;

    if (state_val != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    // keycode do Wayland = keycode do kernel (key) + 8
    xkb_keysym_t sym = xkb_state_key_get_one_sym(app->xkb_state, key + 8);

    if (sym == XKB_KEY_Escape) {
        app->running = false;
        return;
    }

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (app->nfiltered > 0 && app->sel < (int)app->nfiltered) {
            App *selected_app = &app->apps[app->filtered[app->sel]];
            LOG_INFO("Executando aplicativo: %s (%s)", selected_app->name, selected_app->exec);
            spawn_app(selected_app);
        }
        app->running = false;
        return;
    }

    if (sym == XKB_KEY_Down) {
        if (app->focus_zone == ZONE_SEARCH) {
            app->focus_zone = ZONE_LIST;
            app->sel = 0;
        } else if (app->focus_zone == ZONE_LIST) {
            app->sel = (app->sel + 1 < (int)app->nfiltered) ? app->sel + 1 : (int)app->nfiltered - 1;
        }
        app->dirty = true;
        return;
    }

    if (sym == XKB_KEY_Up) {
        if (app->focus_zone == ZONE_LIST) {
            if (app->sel == 0) {
                app->focus_zone = ZONE_SEARCH;
            } else {
                app->sel--;
            }
        }
        app->dirty = true;
        return;
    }

    if (sym == XKB_KEY_BackSpace) {
        if (app->query_len > 0) {
            do {
                app->query_len--;
            } while (app->query_len > 0 && ((unsigned char)app->query[app->query_len] & 0xC0) == 0x80);
            app->query[app->query_len] = '\0';
            app->focus_zone = ZONE_SEARCH;
            app->sel = 0;
            filter_apps();
            app->dirty = true;
        }
        return;
    }

    // Alimentação e consulta do Compose State (dead keys / acentuação)
    enum xkb_compose_status status = XKB_COMPOSE_NOTHING;
    if (app->xkb_compose_state) {
        xkb_compose_state_feed(app->xkb_compose_state, sym);
        status = xkb_compose_state_get_status(app->xkb_compose_state);
    }

    if (status == XKB_COMPOSE_COMPOSED) {
        char utf8[8];
        int n = xkb_compose_state_get_utf8(app->xkb_compose_state, utf8, sizeof(utf8));
        if (n > 0 && (unsigned char)utf8[0] >= 32 && app->query_len + (size_t)n < sizeof(app->query)) {
            memcpy(app->query + app->query_len, utf8, n);
            app->query_len += n;
            app->query[app->query_len] = '\0';
            app->focus_zone = ZONE_SEARCH;
            app->sel = 0;
            filter_apps();
            app->dirty = true;
        }
        xkb_compose_state_reset(app->xkb_compose_state);
        return;
    } else if (status == XKB_COMPOSE_COMPOSING) {
        // Aguarda a composição ser completada (ex: pressionou '´')
        return;
    } else if (status == XKB_COMPOSE_CANCELLED) {
        xkb_compose_state_reset(app->xkb_compose_state);
    }

    // Caractere imprimível normal (caso não seja sequência de compose ou compose tenha sido cancelado)
    char utf8[8];
    int n = xkb_state_key_get_utf8(app->xkb_state, key + 8, utf8, sizeof(utf8));
    if (n > 0 && (unsigned char)utf8[0] >= 32 && app->query_len + (size_t)n < sizeof(app->query)) {
        memcpy(app->query + app->query_len, utf8, n);
        app->query_len += n;
        app->query[app->query_len] = '\0';
        app->focus_zone = ZONE_SEARCH;
        app->sel = 0;
        filter_apps();
        app->dirty = true;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    (void)kbd; (void)serial;
    AppState *app = &g_app;
    if (app->xkb_state) {
        xkb_state_update_mask(app->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
    }
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate, int32_t delay) {
    (void)data; (void)kbd; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// --- Callbacks de Pointer (Mouse) ---
static void pointer_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p; (void)s;
    g_app.ptr_surface = surf;
    g_app.ptr_x = wl_fixed_to_double(sx);
    g_app.ptr_y = wl_fixed_to_double(sy);
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf) {
    (void)d; (void)p; (void)s; (void)surf;
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p; (void)t;
    AppState *app = &g_app;
    app->ptr_x = wl_fixed_to_double(sx);
    app->ptr_y = wl_fixed_to_double(sy);

    if (app->ptr_surface == app->menu_surf) {
        double x = app->ptr_x;
        double y = app->ptr_y;
        if (x >= 12 && x <= app->menu_w - 12) {
            if (y >= 56 && y < 56 + (MAX_ITEMS * ITEM_HEIGHT)) {
                int hover_idx = app->scroll_offset + (int)((y - 56) / ITEM_HEIGHT);
                if (hover_idx >= 0 && hover_idx < (int)app->nfiltered) {
                    if (app->focus_zone != ZONE_LIST || app->sel != hover_idx) {
                        app->focus_zone = ZONE_LIST;
                        app->sel = hover_idx;
                        app->dirty = true;
                    }
                }
            } else if (y >= 12 && y <= 46) {
                if (app->focus_zone != ZONE_SEARCH) {
                    app->focus_zone = ZONE_SEARCH;
                    app->dirty = true;
                }
            }
        }
    }
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t btn, uint32_t state) {
    (void)d; (void)p; (void)s; (void)t;
    AppState *app = &g_app;
    if (state == 1 && btn == 0x110) { // 1 = WL_POINTER_BUTTON_STATE_PRESSED, 0x110 = BTN_LEFT
        if (app->ptr_surface == app->menu_surf) {
            double x = app->ptr_x;
            double y = app->ptr_y;
            // Verifica se clicou na área da lista (x >= 12 && x <= menu_w - 12)
            if (x >= 12 && x <= app->menu_w - 12) {
                if (y >= 56 && y < 56 + (MAX_ITEMS * ITEM_HEIGHT)) {
                    int clicked_idx = app->scroll_offset + (int)((y - 56) / ITEM_HEIGHT);
                    if (clicked_idx >= 0 && clicked_idx < (int)app->nfiltered) {
                        App *selected_app = &app->apps[app->filtered[clicked_idx]];
                        LOG_INFO("Mouse: clicou no aplicativo %s (%s)", selected_app->name, selected_app->exec);
                        spawn_app(selected_app);
                        app->running = false;
                    }
                }
            }
        }
    }
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t ax, wl_fixed_t val) {
    (void)d; (void)p; (void)t; (void)ax; (void)val;
}
static void pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    (void)data; (void)wl_pointer;
}
static void pointer_axis_source(void *data, struct wl_pointer *wl_pointer, uint32_t axis_source) {
    (void)data; (void)wl_pointer; (void)axis_source;
}
static void pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis) {
    (void)data; (void)wl_pointer; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t discrete) {
    (void)data; (void)wl_pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

// --- Callbacks de Assento (Seat) ---
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    AppState *app = &g_app;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && app->keyboard) {
        wl_keyboard_destroy(app->keyboard);
        app->keyboard = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && app->pointer) {
        wl_pointer_destroy(app->pointer);
        app->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

// --- Callbacks do Registry ---
static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)data;
    AppState *app = &g_app;
    if (strcmp(iface, "wl_compositor") == 0) {
        app->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(iface, "wl_shm") == 0) {
        app->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, "wl_seat") == 0) {
        app->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 7 ? version : 7);
    } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
        app->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    } else if (strcmp(iface, "wp_viewporter") == 0) {
        app->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// --- Callbacks da Superfície do Menu ---
static void menu_layer_configure(void *data, struct zwlr_layer_surface_v1 *surf, uint32_t serial, uint32_t w, uint32_t h) {
    (void)data;
    AppState *app = &g_app;
    zwlr_layer_surface_v1_ack_configure(surf, serial);

    int new_w = w > 0 ? (int)w : MENU_WIDTH;
    int new_h = h > 0 ? (int)h : MENU_HEIGHT;

    if (!app->configured || new_w != app->menu_w || new_h != app->menu_h) {
        app->menu_w = new_w;
        app->menu_h = new_h;

        // Desaloca buffers antigos se redimensionado
        if (app->shm_data) {
            munmap(app->shm_data, app->buf_size * 2);
            wl_buffer_destroy(app->buffers[0]);
            wl_buffer_destroy(app->buffers[1]);
            wl_shm_pool_destroy(app->shm_pool);
            close(app->shm_fd);
            app->shm_data = NULL;
        }

        create_menu_buffers(new_w, new_h);
        app->configured = true;
    }
    app->dirty = true;
}

static void menu_layer_closed(void *data, struct zwlr_layer_surface_v1 *surf) {
    (void)data; (void)surf;
    LOG_INFO("menu: layer surface closed");
    g_app.running = false;
}

static const struct zwlr_layer_surface_v1_listener menu_layer_listener = {
    .configure = menu_layer_configure,
    .closed = menu_layer_closed,
};

static void create_menu_surface(void) {
    AppState *app = &g_app;
    app->menu_surf = wl_compositor_create_surface(app->compositor);
    if (!app->menu_surf) {
        LOG_ERR("wl_compositor_create_surface failed");
        exit(1);
    }

    app->menu_layer = zwlr_layer_shell_v1_get_layer_surface(app->layer_shell, app->menu_surf, NULL,
                        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "maindeck-menu");
    if (!app->menu_layer) {
        LOG_ERR("get_layer_surface failed");
        exit(1);
    }

    zwlr_layer_surface_v1_set_anchor(app->menu_layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_margin(app->menu_layer, 0, 0, 2, 6); // margens: x=6, y=2 (alinhado com o botao iniciar verde)
    zwlr_layer_surface_v1_set_size(app->menu_layer, MENU_WIDTH, MENU_HEIGHT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(app->menu_layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    zwlr_layer_surface_v1_add_listener(app->menu_layer, &menu_layer_listener, NULL);

    wl_surface_commit(app->menu_surf);
}

// --- Main ---
int main(int argc, char **argv) {
    (void)argc; (void)argv;
#ifdef __GLIBC__
    mallopt(M_ARENA_MAX, 1);
#endif
    struct rlimit stk = { 2 * 1024 * 1024, 2 * 1024 * 1024 };
    setrlimit(RLIMIT_STACK, &stk);

    srand((unsigned int)time(NULL));

    memset(&g_app, 0, sizeof(AppState));
    g_app.focus_timer_fd = -1;
    g_app.shm_fd = -1;
    g_app.bg_shm_fd = -1;
    g_app.running = true;
    g_app.focus_zone = ZONE_SEARCH;

    // 1. IPC / Instância única
    if (!setup_ipc()) {
        return 1;
    }

    LOG_INFO("Inicializando maindeck-menu...");

    // 2. Carrega lista de aplicativos
    load_all_apps();
    LOG_INFO("Carregados %zu aplicativos", g_app.napps);

    g_app.filtered = malloc(g_app.napps * sizeof(int));
    g_app.scores = malloc(g_app.napps * sizeof(int));
    filter_apps();

    // 3. Inicializa contexto XKB e Wayland
    g_app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (g_app.xkb_context) {
        const char *locale = getenv("LC_ALL");
        if (!locale) locale = getenv("LC_CTYPE");
        if (!locale) locale = getenv("LANG");
        if (!locale) locale = "pt_BR.UTF-8";

        g_app.xkb_compose_table = xkb_compose_table_new_from_locale(g_app.xkb_context, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (g_app.xkb_compose_table) {
            g_app.xkb_compose_state = xkb_compose_state_new(g_app.xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        } else {
            LOG_ERR("Falha ao criar xkb_compose_table");
        }
    }
    g_app.display = wl_display_connect(NULL);
    if (!g_app.display) {
        LOG_ERR("cannot connect to Wayland display");
        cleanup_ipc();
        return 1;
    }

    g_app.registry = wl_display_get_registry(g_app.display);
    wl_registry_add_listener(g_app.registry, &registry_listener, NULL);
    wl_display_roundtrip(g_app.display);

    if (!g_app.compositor || !g_app.shm || !g_app.layer_shell) {
        LOG_ERR("missing required Wayland globals");
        wl_display_disconnect(g_app.display);
        cleanup_ipc();
        return 1;
    }

    if (g_app.seat) {
        wl_seat_add_listener(g_app.seat, &seat_listener, NULL);
    }

    // 4. Cria superfície MENU
    create_menu_surface();
    wl_display_roundtrip(g_app.display);

    // 5. Fallback de foco desativado (usando ON_DEMAND puro sem bg_surf)
    g_app.focus_timer_fd = -1;

    // 6. Loop de eventos principal (prepare_read)
    int wl_fd = wl_display_get_fd(g_app.display);

    while (g_app.running) {
        while (wl_display_prepare_read(g_app.display) != 0) {
            wl_display_dispatch_pending(g_app.display);
        }

        if (wl_display_flush(g_app.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g_app.display);
            break;
        }

        struct pollfd fds[2] = {
            { .fd = wl_fd, .events = POLLIN },
            { .fd = g_app.sock_fd, .events = POLLIN }
        };

        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            wl_display_cancel_read(g_app.display);
            if (errno == EINTR) continue;
            break;
        }

        // Drena eventos Wayland se houver
        if (fds[0].revents & POLLIN) {
            wl_display_read_events(g_app.display);
            wl_display_dispatch_pending(g_app.display);
        } else {
            wl_display_cancel_read(g_app.display);
        }

        // Recebeu datagrama no Socket IPC -> Toggle Close
        if (fds[1].revents & POLLIN) {
            char buf[16];
            recv(g_app.sock_fd, buf, sizeof(buf), 0);
            LOG_INFO("IPC: Comando CLOSE recebido no socket");
            g_app.running = false;
        }



        // Desenha se mudou algo
        if (g_app.dirty && g_app.configured) {
            render();
            g_app.dirty = false;
        }
    }

    LOG_INFO("Encerrando maindeck-menu...");

    if (g_app.focus_timer_fd >= 0) close(g_app.focus_timer_fd);

    if (g_app.shm_data) munmap(g_app.shm_data, g_app.buf_size * 2);
    if (g_app.buffers[0]) wl_buffer_destroy(g_app.buffers[0]);
    if (g_app.buffers[1]) wl_buffer_destroy(g_app.buffers[1]);
    if (g_app.shm_pool) wl_shm_pool_destroy(g_app.shm_pool);
    if (g_app.shm_fd >= 0) close(g_app.shm_fd);

    if (g_app.menu_layer) zwlr_layer_surface_v1_destroy(g_app.menu_layer);
    if (g_app.menu_surf) wl_surface_destroy(g_app.menu_surf);

    if (g_app.keyboard) wl_keyboard_destroy(g_app.keyboard);
    if (g_app.pointer) wl_pointer_destroy(g_app.pointer);
    if (g_app.xkb_compose_state) xkb_compose_state_unref(g_app.xkb_compose_state);
    if (g_app.xkb_compose_table) xkb_compose_table_unref(g_app.xkb_compose_table);
    if (g_app.xkb_state) xkb_state_unref(g_app.xkb_state);
    if (g_app.xkb_keymap) xkb_keymap_unref(g_app.xkb_keymap);
    if (g_app.xkb_context) xkb_context_unref(g_app.xkb_context);

    wl_registry_destroy(g_app.registry);
    wl_display_disconnect(g_app.display);

    cleanup_ipc();

    for (size_t i = 0; i < g_app.napps; i++) {
        free(g_app.apps[i].id);
        free(g_app.apps[i].name);
        free(g_app.apps[i].exec);
        free(g_app.apps[i].icon);
    }
    free(g_app.apps);
    free(g_app.filtered);
    free(g_app.scores);

    for (int i = 0; i < 2; i++) {
        if (s_lay[i]) { g_object_unref(s_lay[i]); s_lay[i] = NULL; }
        if (s_cr[i])  { cairo_destroy(s_cr[i]); s_cr[i] = NULL; }
        if (s_cs[i])  { cairo_surface_destroy(s_cs[i]); s_cs[i] = NULL; }
    }
    if (s_font_desc) {
        pango_font_description_free(s_font_desc);
        s_font_desc = NULL;
    }

    return 0;
}
