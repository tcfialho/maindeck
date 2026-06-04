#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
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

/* ------------------------------------------------------------------ */
/* PNG search in icon theme                                             */
/* ------------------------------------------------------------------ */

static int find_icon_path(const char *icon_name, int size, char *path_out, size_t cap) {
    /* Try exact size first, then common sizes */
    static const int sizes[] = { 32, 24, 48, 16, 64, 128, 256, 0 };

    /* Directories to search */
    const char *themes[] = { "hicolor", "breeze", "breeze-dark", "Papirus", NULL };
    const char *base_dirs[] = {
        "/usr/share/icons",
        NULL
    };

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
    snprintf(path_out, cap, "/usr/share/pixmaps/%s.png", icon_name);
    if (access(path_out, R_OK) == 0) return 0;
    snprintf(path_out, cap, "/usr/share/pixmaps/%s.xpm", icon_name);
    if (access(path_out, R_OK) == 0) return 0;

    /* App-specific fallback paths */
    snprintf(path_out, cap, "/usr/share/sunshine/web/images/%s-45.png", icon_name);
    if (access(path_out, R_OK) == 0) return 0;
    snprintf(path_out, cap, "/usr/share/sunshine/web/images/%s-16.png", icon_name);
    if (access(path_out, R_OK) == 0) return 0;
    snprintf(path_out, cap, "/usr/share/sunshine/web/images/%s.png", icon_name);
    if (access(path_out, R_OK) == 0) return 0;
    snprintf(path_out, cap, "/usr/share/sunshine/web/images/%s.svg", icon_name);
    if (access(path_out, R_OK) == 0) return 0;

    return -1;
}

/* ------------------------------------------------------------------ */
/* gdk-pixbuf → cairo_surface_t                                        */
/* ------------------------------------------------------------------ */

static cairo_surface_t *pixbuf_to_cairo(const char *path, int size) {
    GError     *err  = NULL;
    GdkPixbuf  *pb   = gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, &err);
    if (!pb) {
        if (err) { g_error_free(err); }
        return NULL;
    }

    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    int nc = gdk_pixbuf_get_n_channels(pb);
    guchar *pixels = gdk_pixbuf_get_pixels(pb);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(pb);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    int crs = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            guchar *src = pixels + y * rs + x * nc;
            unsigned char r = src[0];
            unsigned char g = src[1];
            unsigned char b = src[2];
            unsigned char a = has_alpha ? src[3] : 255;
            /* Premultiply alpha for CAIRO_FORMAT_ARGB32 */
            unsigned char *dst = data + y * crs + x * 4;
            dst[0] = (unsigned char)((b * a) / 255);
            dst[1] = (unsigned char)((g * a) / 255);
            dst[2] = (unsigned char)((r * a) / 255);
            dst[3] = a;
        }
    }

    cairo_surface_mark_dirty(surf);
    g_object_unref(pb);
    return surf;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

char *bar_icon_nf_glyph(const char *name) {
    if (strncmp(name, "nf:", 3) == 0) {
        return strdup(name + 3);
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
        surf = pixbuf_to_cairo(path, size);
        if (!surf) {
            /* Try as app_id → .desktop lookup */
            char desktop_icon[128] = "";
            if (read_desktop_icon(name, desktop_icon, sizeof(desktop_icon)) == 0) {
                if (find_icon_path(desktop_icon, size, path, sizeof(path)) == 0)
                    surf = pixbuf_to_cairo(path, size);
            }
        }
    } else {
        /* Try .desktop lookup */
        char desktop_icon[128] = "";
        if (read_desktop_icon(name, desktop_icon, sizeof(desktop_icon)) == 0) {
            if (find_icon_path(desktop_icon, size, path, sizeof(path)) == 0)
                surf = pixbuf_to_cairo(path, size);
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
            surf = pixbuf_to_cairo(path, size);

        /* lowercase */
        for (char *p = short_name; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;

        if (!surf && find_icon_path(short_name, size, path, sizeof(path)) == 0)
            surf = pixbuf_to_cairo(path, size);

        /* strip trailing -state suffix */
        if (!surf) {
            char *dash = strchr(short_name, '-');
            if (dash) {
                char base[128];
                snprintf(base, sizeof(base), "%.*s", (int)(dash - short_name), short_name);
                if (find_icon_path(base, size, path, sizeof(path)) == 0)
                    surf = pixbuf_to_cairo(path, size);
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
