#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <librsvg/rsvg.h>
#include <cairo/cairo.h>

#include "bar-icons.h"
#include "bar-log.h"

#define ICON_CACHE_MAX 64

struct IconEntry {
    char              name[128];
    cairo_surface_t  *surf;   /* NULL = nf: glyph or not found */
    char              glyph[32]; /* set if nf: prefix */
};

static struct IconEntry g_cache[ICON_CACHE_MAX];
static int              g_cache_n = 0;

/* ------------------------------------------------------------------ */
/* .desktop lookup                                                      */
/* ------------------------------------------------------------------ */

static int read_desktop_icon(const char *app_id, char *icon_out, size_t cap) {
    char path[512];
    FILE *f = NULL;

    /* Try ~/.local/share first */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.local/share/applications/%s.desktop", home, app_id);
        f = fopen(path, "r");
    }
    if (!f) {
        snprintf(path, sizeof(path), "/usr/share/applications/%s.desktop", app_id);
        f = fopen(path, "r");
    }
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Icon=", 5) == 0) {
            size_t l = strlen(line + 5);
            while (l > 0 && (line[5+l-1] == '\n' || line[5+l-1] == '\r')) l--;
            if (l >= cap) l = cap - 1;
            memcpy(icon_out, line + 5, l);
            icon_out[l] = '\0';
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static bool scan_dir_recursive(const char *dir_path, const char *icon_name, char *path_out, size_t cap) {
    DIR *dir = opendir(dir_path);
    if (!dir) return false;

    struct dirent *entry;
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (lstat(full_path, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                continue; // Pula links simbólicos para evitar loops infinitos
            }
            if (S_ISDIR(st.st_mode)) {
                if (scan_dir_recursive(full_path, icon_name, path_out, cap)) {
                    found = true;
                    break;
                }
            } else if (S_ISREG(st.st_mode)) {
                const char *filename = entry->d_name;
                size_t len = strlen(filename);
                size_t name_len = strlen(icon_name);
                if (len > name_len && strncmp(filename, icon_name, name_len) == 0) {
                    const char *ext = filename + name_len;
                    if (strcmp(ext, ".png") == 0 || strcmp(ext, ".svg") == 0 || strcmp(ext, ".xpm") == 0) {
                        snprintf(path_out, cap, "%s", full_path);
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    closedir(dir);
    return found;
}

/* ------------------------------------------------------------------ */
/* PNG search in icon theme                                             */
/* ------------------------------------------------------------------ */

static int find_icon_path(const char *icon_name, int size, char *path_out, size_t cap) {
    /* Try exact size first, then common sizes */
    static const int sizes[] = { 32, 24, 48, 16, 64, 128, 256, 0 };

    /* Directories to search */
    const char *themes[] = { "hicolor", "breeze", "breeze-dark", "Papirus", NULL };
    
    char local_icons_dir[512] = "";
    char local_pixmaps_dir[512] = "";
    const char *home = getenv("HOME");
    if (home) {
        snprintf(local_icons_dir, sizeof(local_icons_dir), "%s/.local/share/icons", home);
        snprintf(local_pixmaps_dir, sizeof(local_pixmaps_dir), "%s/.local/share/pixmaps", home);
    }

    const char *base_dirs[5];
    int base_dirs_n = 0;
    if (local_icons_dir[0]) {
        base_dirs[base_dirs_n++] = local_icons_dir;
    }
    base_dirs[base_dirs_n++] = "/usr/share/icons";
    base_dirs[base_dirs_n++] = "/usr/local/share/icons";
    base_dirs[base_dirs_n++] = "/var/lib/flatpak/exports/share/icons";
    base_dirs[base_dirs_n++] = NULL;

    /* If icon_name is an absolute path */
    if (icon_name[0] == '/') {
        snprintf(path_out, cap, "%s", icon_name);
        return 0;
    }

    static const char *categories[] = { "apps", "places", "actions", NULL };

    for (int b = 0; base_dirs[b]; b++) {
        for (int th = 0; themes[th]; th++) {
            for (int cat = 0; categories[cat]; cat++) {
                for (int si = 0; sizes[si]; si++) {
                    int sz = (si == 0) ? size : sizes[si];
                    /* PNG: theme/NxN/cat/icon.png */
                    snprintf(path_out, cap, "%s/%s/%dx%d/%s/%s.png",
                             base_dirs[b], themes[th], sz, sz, categories[cat], icon_name);
                    if (access(path_out, R_OK) == 0) return 0;
                    /* SVG: theme/cat/N/icon.svg  (breeze layout) */
                    snprintf(path_out, cap, "%s/%s/%s/%d/%s.svg",
                             base_dirs[b], themes[th], categories[cat], sz, icon_name);
                    if (access(path_out, R_OK) == 0) return 0;
                }
                /* SVG: theme/scalable/cat/icon.svg */
                snprintf(path_out, cap, "%s/%s/scalable/%s/%s.svg",
                         base_dirs[b], themes[th], categories[cat], icon_name);
                if (access(path_out, R_OK) == 0) return 0;
            }
        }
    }
    /* Pixmaps fallback */
    if (local_pixmaps_dir[0]) {
        snprintf(path_out, cap, "%s/%s.png", local_pixmaps_dir, icon_name);
        if (access(path_out, R_OK) == 0) return 0;
        snprintf(path_out, cap, "%s/%s.xpm", local_pixmaps_dir, icon_name);
        if (access(path_out, R_OK) == 0) return 0;
    }
    snprintf(path_out, cap, "/usr/share/pixmaps/%s.png", icon_name);
    if (access(path_out, R_OK) == 0) return 0;
    snprintf(path_out, cap, "/usr/share/pixmaps/%s.xpm", icon_name);
    if (access(path_out, R_OK) == 0) return 0;

    /* App-specific fallback paths */
    snprintf(path_out, cap, "/usr/share/sunshine/web/images/%s-45.png", icon_name);
    if (access(path_out, R_OK) == 0) return 0;
    snprintf(path_out, cap, "/usr/share/sunshine/web/images/%s-16.png", icon_name);
    if (access(path_out, R_OK) == 0) return 0;
    snprintf(path_out, cap, "/usr/share/sunshine/web/images/%s.svg", icon_name);
    if (access(path_out, R_OK) == 0) return 0;

    /* Varredura recursiva robusta nos caminhos de icones como ultimo recurso */
    for (int b = 0; base_dirs[b]; b++) {
        if (scan_dir_recursive(base_dirs[b], icon_name, path_out, cap)) {
            return 0;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Native SVG/PNG to cairo_surface_t                                 */
/* ------------------------------------------------------------------ */

static cairo_surface_t *load_icon_to_cairo_surface(const char *path, int size) {
    cairo_surface_t *surf = NULL;
    size_t len = strlen(path);

    bool is_svg = false;
    if (len > 4) {
        if (strcasecmp(path + len - 4, ".svg") == 0) {
            is_svg = true;
        }
    }

    if (is_svg) {
        GError *error = NULL;
        RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
        if (!handle) {
            if (error) {
                LOG_WARN("icons: librsvg failed to load %s: %s", path, error->message);
                g_error_free(error);
            }
            return NULL;
        }

        surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t *cr = cairo_create(surf);

        RsvgRectangle viewport = {
            .x = 0.0,
            .y = 0.0,
            .width = (double)size,
            .height = (double)size
        };

        gboolean ok = rsvg_handle_render_document(handle, cr, &viewport, &error);
        if (!ok) {
            if (error) {
                LOG_WARN("icons: librsvg failed to render %s: %s", path, error->message);
                g_error_free(error);
            }
            cairo_destroy(cr);
            cairo_surface_destroy(surf);
            g_object_unref(handle);
            return NULL;
        }

        cairo_destroy(cr);
        g_object_unref(handle);
    } else {
        cairo_surface_t *img = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(img);
            return NULL;
        }

        int w = cairo_image_surface_get_width(img);
        int h = cairo_image_surface_get_height(img);

        if (w == size && h == size) {
            return img;
        }

        surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t *cr = cairo_create(surf);
        
        cairo_scale(cr, (double)size / w, (double)size / h);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        
        cairo_destroy(cr);
        cairo_surface_destroy(img);
    }

    return surf;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

const char *bar_icon_nf_glyph(const char *name) {
    if (strncmp(name, "nf:", 3) == 0) {
        return name + 3;
    }
    return NULL;
}

cairo_surface_t *bar_icon_get(const char *name, int size) {
    if (!name || !name[0]) return NULL;

    /* nf: prefix → caller renders text, we return NULL */
    if (strncmp(name, "nf:", 3) == 0) return NULL;

    /* Check cache */
    for (int i = 0; i < g_cache_n; i++) {
        if (strcmp(g_cache[i].name, name) == 0)
            return g_cache[i].surf;
    }

    /* Not in cache — resolve */
    char icon_name[128];
    /* First try name directly as icon name */
    snprintf(icon_name, sizeof(icon_name), "%s", name);

    char path[512] = "";
    cairo_surface_t *surf = NULL;

    if (find_icon_path(icon_name, size, path, sizeof(path)) == 0) {
        surf = load_icon_to_cairo_surface(path, size);
        if (!surf) {
            /* Try as app_id → .desktop lookup */
            char desktop_icon[128] = "";
            if (read_desktop_icon(name, desktop_icon, sizeof(desktop_icon)) == 0) {
                if (find_icon_path(desktop_icon, size, path, sizeof(path)) == 0)
                    surf = load_icon_to_cairo_surface(path, size);
            }
        }
    } else {
        /* Try .desktop lookup */
        char desktop_icon[128] = "";
        if (read_desktop_icon(name, desktop_icon, sizeof(desktop_icon)) == 0) {
            if (find_icon_path(desktop_icon, size, path, sizeof(path)) == 0)
                surf = load_icon_to_cairo_surface(path, size);
        }
    }

    /* Last resort: if name looks like reverse-DNS (contains dots), try the
     * last component as a plain icon name.
     * "dev.lizardbyte.app.Sunshine-playing" → try "Sunshine-playing",
     * "sunshine-playing", then "sunshine". */
    if (!surf && strchr(name, '.')) {
        const char *last_dot = strrchr(name, '.');
        char short_name[128];
        snprintf(short_name, sizeof(short_name), "%s", last_dot + 1);

        /* Try original case first */
        if (find_icon_path(short_name, size, path, sizeof(path)) == 0)
            surf = load_icon_to_cairo_surface(path, size);

        /* lowercase */
        for (char *p = short_name; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;

        if (!surf && find_icon_path(short_name, size, path, sizeof(path)) == 0)
            surf = load_icon_to_cairo_surface(path, size);

        /* strip trailing -state suffix */
        if (!surf) {
            char *dash = strchr(short_name, '-');
            if (dash) {
                char base[128];
                snprintf(base, sizeof(base), "%.*s", (int)(dash - short_name), short_name);
                if (find_icon_path(base, size, path, sizeof(path)) == 0)
                    surf = load_icon_to_cairo_surface(path, size);
            }
        }
    }

    if (!surf) {
        LOG_WARN("icons: no icon found for '%s'", name);
    }

    /* Store in cache (even if NULL to avoid repeated lookups) */
    if (g_cache_n < ICON_CACHE_MAX) {
        snprintf(g_cache[g_cache_n].name, sizeof(g_cache[g_cache_n].name), "%s", name);
        g_cache[g_cache_n].surf = surf;
        g_cache_n++;
    }

    return surf;
}

void bar_icon_draw(cairo_t *cr, cairo_surface_t *icon, double x, double y, int size) {
    if (!icon) {
        /* placeholder: small square */
        cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 0.5);
        cairo_rectangle(cr, x, y, size, size);
        cairo_fill(cr);
        return;
    }
    cairo_set_source_surface(cr, icon, x, y);
    cairo_paint(cr);
}

void bar_icons_cleanup(void) {
    for (int i = 0; i < g_cache_n; i++) {
        if (g_cache[i].surf) {
            cairo_surface_destroy(g_cache[i].surf);
            g_cache[i].surf = NULL;
        }
    }
    g_cache_n = 0;
}
