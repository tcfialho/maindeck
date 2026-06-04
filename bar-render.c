#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "bar-state.h"
#include "bar-render.h"
#include "bar-surface.h"
#include "bar-icons.h"
#include "bar-tray.h"
#include "bar-log.h"

/* ------------------------------------------------------------------ */
/* Colors (RGBA, pre-multiplied handled by cairo)                       */
/* ------------------------------------------------------------------ */

#define COL_BG          0.11, 0.11, 0.14, 1.0   /* #1C1C24 */
#define COL_BTN_NORMAL  0.15, 0.15, 0.20, 1.0
#define COL_BTN_HOVER   0.22, 0.22, 0.30, 1.0
#define COL_ACTIVE      0.18, 0.36, 0.70, 1.0   /* accent blue */
#define COL_TEXT        0.92, 0.92, 0.92, 1.0
#define COL_TEXT_DIM    0.55, 0.55, 0.60, 1.0
#define COL_SEPARATOR   0.25, 0.25, 0.30, 0.8

#define ICON_SIZE       18
#define BTN_PAD         8
#define BTN_RADIUS      4.0

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void set_col(cairo_t *cr, double r, double g, double b, double a) {
    cairo_set_source_rgba(cr, r, g, b, a);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x+w-r, y+r,   r, -M_PI/2, 0);
    cairo_arc(cr, x+w-r, y+h-r, r, 0,       M_PI/2);
    cairo_arc(cr, x+r,   y+h-r, r, M_PI/2,  M_PI);
    cairo_arc(cr, x+r,   y+r,   r, M_PI,    3*M_PI/2);
    cairo_close_path(cr);
}

static PangoLayout *make_layout(cairo_t *cr, const char *font_desc) {
    PangoLayout    *lay = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(font_desc);
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    return lay;
}

static void draw_text_centered(cairo_t *cr, PangoLayout *lay,
    const char *text, double x, double y, double max_w, double h) {
    pango_layout_set_text(lay, text, -1);
    pango_layout_set_width(lay, (int)(max_w * PANGO_SCALE));
    pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);

    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    cairo_move_to(cr, x + (max_w - tw) / 2.0, y + (h - th) / 2.0);
    pango_cairo_show_layout(cr, lay);
}

/* ------------------------------------------------------------------ */
/* Register hit area                                                     */
/* ------------------------------------------------------------------ */

static void push_hit(HitType type, int idx, int x, int y, int w, int h) {
    struct BarState *bar = &g_bar;
    if (bar->hit_n >= BAR_MAX_HITS) return;
    struct HitArea *ha = &bar->hit_areas[bar->hit_n++];
    ha->type  = type;
    ha->index = idx;
    ha->x = x; ha->y = y; ha->w = w; ha->h = h;
}

/* ------------------------------------------------------------------ */
/* Draw separator                                                        */
/* ------------------------------------------------------------------ */

static void draw_sep(cairo_t *cr, double x, int h) {
    set_col(cr, COL_SEPARATOR);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x + 0.5, 4);
    cairo_line_to(cr, x + 0.5, h - 4);
    cairo_stroke(cr);
}

/* ------------------------------------------------------------------ */
/* Draw quick-launch section                                            */
/* ------------------------------------------------------------------ */

static int draw_quicklaunch(cairo_t *cr, PangoLayout *lay, int h) {
    struct BarState *bar = &g_bar;
    int x = 4;
    int btn_h = h - 4;
    int btn_y = 2;

    for (int i = 0; i < bar->config.ql_count; i++) {
        struct BarQLButton *btn = &bar->config.ql[i];
        bool hovered = (bar->hit_n > 0) && (bar->hover_hit >= 0)
            && (bar->hit_areas[bar->hover_hit].type == HIT_QL)
            && (bar->hit_areas[bar->hover_hit].index == i);

        /* Calculate button width */
        char *glyph = bar_icon_nf_glyph(btn->icon);
        cairo_surface_t *icon = glyph ? NULL : bar_icon_get(btn->icon, ICON_SIZE);

        int btn_w;
        if (glyph) {
            pango_layout_set_text(lay, glyph, -1);
            int tw, th; (void)th;
            pango_layout_get_pixel_size(lay, &tw, &th);
            btn_w = tw + BTN_PAD * 2;
        } else {
            btn_w = ICON_SIZE + BTN_PAD * 2;
        }
        if (btn->width >= 2)
            btn_w = btn_w * btn->width;

        /* Button background: only show on hover (flat by default) */
        if (hovered) {
            set_col(cr, COL_BTN_HOVER);
            rounded_rect(cr, x, btn_y, btn_w, btn_h, BTN_RADIUS);
            cairo_fill(cr);
        }

        /* Draw icon or glyph */
        if (glyph) {
            set_col(cr, COL_TEXT);
            pango_layout_set_text(lay, glyph, -1);
            int tw, th;
            pango_layout_get_pixel_size(lay, &tw, &th);
            cairo_move_to(cr, x + (btn_w - tw) / 2.0, btn_y + (btn_h - th) / 2.0);
            pango_cairo_show_layout(cr, lay);
            free(glyph);
        } else if (icon) {
            double ix = x + (btn_w - ICON_SIZE) / 2.0;
            double iy = btn_y + (btn_h - ICON_SIZE) / 2.0;
            bar_icon_draw(cr, icon, ix, iy, ICON_SIZE);
        } else {
            /* placeholder */
            set_col(cr, 0.3, 0.3, 0.35, 0.7);
            cairo_rectangle(cr,
                x + (btn_w - ICON_SIZE) / 2.0,
                btn_y + (btn_h - ICON_SIZE) / 2.0,
                ICON_SIZE, ICON_SIZE);
            cairo_fill(cr);
        }

        /* Register hit area (before resetting hit_n) */
        push_hit(HIT_QL, i, x, btn_y, btn_w, btn_h);

        x += btn_w + 2;
    }

    return x; /* next x position */
}

/* ------------------------------------------------------------------ */
/* Draw taskbar section                                                  */
/* ------------------------------------------------------------------ */

static void draw_taskbar(cairo_t *cr, PangoLayout *lay, int x_start, int x_end, int h) {
    struct BarState *bar = &g_bar;
    int n = bar->toplevel_n;
    if (n == 0) return;

    int avail  = x_end - x_start - 4;
    int max_btn_w = 160;
    int btn_w  = (n > 0) ? (avail / n) : 0;
    if (btn_w > max_btn_w) btn_w = max_btn_w;
    if (btn_w < 32) btn_w = 32;

    int btn_h = h - 4;
    int btn_y = 2;
    int x = x_start + 4;

    for (int i = 0; i < n; i++) {
        struct BarToplevel *tl = &bar->toplevels[i];

        bool hovered = (bar->hover_hit >= 0) && (bar->hit_n > 0)
            && (bar->hit_areas[bar->hover_hit].type == HIT_TASKBAR)
            && (bar->hit_areas[bar->hover_hit].index == i);

        /* Hover: subtle background fill */
        if (hovered && !tl->activated) {
            set_col(cr, COL_BTN_HOVER);
            rounded_rect(cr, x, btn_y, btn_w - 2, btn_h, BTN_RADIUS);
            cairo_fill(cr);
        }

        /* Active: underline at the bottom of the button */
        if (tl->activated) {
            set_col(cr, 0.30, 0.57, 1.0, 1.0);  /* #4D92FF — accent blue */
            double lx = x + 2;
            double lw = btn_w - 6;
            double ly = btn_y + btn_h - 2;
            cairo_rectangle(cr, lx, ly, lw, 2);
            cairo_fill(cr);
        }

        /* Icon */
        int text_x = x + BTN_PAD;
        if (tl->icon_surface) {
            double iy = btn_y + (btn_h - ICON_SIZE) / 2.0;
            bar_icon_draw(cr, tl->icon_surface, text_x, iy, ICON_SIZE);
            text_x += ICON_SIZE + 4;
        }

        /* Title text */
        set_col(cr, COL_TEXT);
        double text_w = (btn_w - 2) - (text_x - x) - BTN_PAD;
        if (text_w > 8) {
            draw_text_centered(cr, lay, tl->title[0] ? tl->title : "...",
                text_x, btn_y, text_w, btn_h);
        }

        push_hit(HIT_TASKBAR, i, x, btn_y, btn_w - 2, btn_h);
        x += btn_w;
    }
}

/* ------------------------------------------------------------------ */
/* Draw status section                                                   */
/* ------------------------------------------------------------------ */

static int draw_tray(cairo_t *cr, int h, int x_end) {
    int n    = bar_tray_count();
    int x    = x_end;
    int sz   = ICON_SIZE;
    int pad  = 4;
    int btn_y = (h - sz) / 2;

    for (int i = n - 1; i >= 0; i--) {
        x -= sz + pad;
        cairo_surface_t *icon = bar_tray_icon(i);
        bar_icon_draw(cr, icon, x, btn_y, sz);
        push_hit(HIT_TRAY, i, x - 2, 0, sz + 4, h);
    }
    if (n > 0) x -= 4; /* gap before status modules */
    return x;
}

static int draw_status(cairo_t *cr, PangoLayout *lay, int h, int x_end) {
    struct BarState *bar = &g_bar;
    int x = x_end;
    int btn_h = h - 4;
    int btn_y = 2;

    for (int i = bar->config.status_n - 1; i >= 0; i--) {
        const char *mod = bar->config.status[i];
        const char *text = NULL;

        if (strcmp(mod, "clock") == 0)   text = bar->clock_text;
        else if (strcmp(mod, "volume") == 0)  text = bar->vol_text;
        else if (strcmp(mod, "battery") == 0) text = bar->bat_text;
        else if (strcmp(mod, "power") == 0)   text = "";

        if (!text) continue;

        bool is_power = (strcmp(mod, "power") == 0);
        const char *display_text = is_power ? "" : text;
        if (is_power) display_text = "⏻";

        pango_layout_set_text(lay, display_text, -1);
        int tw, th; (void)th;
        pango_layout_get_pixel_size(lay, &tw, &th);
        int btn_w = tw + BTN_PAD * 2;
        if (btn_w < 32) btn_w = 32;

        x -= btn_w + 2;

        bool hovered = (bar->hover_hit >= 0) && (bar->hit_n > 0)
            && (bar->hit_areas[bar->hover_hit].type == HIT_STATUS)
            && (bar->hit_areas[bar->hover_hit].index == i);

        if (hovered || is_power) {
            set_col(cr, COL_BTN_HOVER);
            rounded_rect(cr, x, btn_y, btn_w, btn_h, BTN_RADIUS);
            cairo_fill(cr);
        }

        set_col(cr, COL_TEXT);
        pango_layout_set_text(lay, display_text, -1);
        pango_layout_get_pixel_size(lay, &tw, &th);
        cairo_move_to(cr, x + (btn_w - tw) / 2.0, btn_y + (btn_h - th) / 2.0);
        pango_cairo_show_layout(cr, lay);

        push_hit(HIT_STATUS, i, x, btn_y, btn_w, btn_h);
    }
    return x;
}

/* ------------------------------------------------------------------ */
/* Public: full redraw                                                   */
/* ------------------------------------------------------------------ */

void bar_render(void) {
    struct BarState *bar = &g_bar;
    if (!bar->configured || !bar->shm_data) return;

    void *data = bar_surface_get_draw_data();
    if (!data) return;

    int w = bar->buf_width;
    int h = bar->buf_height;

    /* Create cairo surface over the SHM buffer */
    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (unsigned char *)data,
        CAIRO_FORMAT_ARGB32,
        w, h,
        bar->buf_stride
    );
    cairo_t *cr = cairo_create(cs);

    /* Background */
    set_col(cr, COL_BG);
    cairo_paint(cr);

    /* Font layout */
    PangoLayout *lay = make_layout(cr, bar->config.font);

    /* Clear hit areas */
    bar->hit_n = 0;

    /* Quick-launch (left) */
    int ql_end = draw_quicklaunch(cr, lay, h);

    /* Separator */
    draw_sep(cr, ql_end + 2, h);

    /* Status modules (rightmost: power, clock, bat, vol) */
    int status_start = draw_status(cr, lay, h, w - 4);

    /* Tray (left of status modules) */
    int tray_start = draw_tray(cr, h, status_start - 4);

    /* Separator */
    draw_sep(cr, tray_start - 4, h);

    /* Taskbar (center, fills remaining space) */
    draw_taskbar(cr, lay, ql_end + 8, tray_start - 8, h);

    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_finish(cs);
    cairo_surface_destroy(cs);

    bar_surface_commit();
    bar->dirty = false;
}
