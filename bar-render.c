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
#define POWER_BTN_W     34
#define POWER_ICON_Y_OFFSET 2.5

static PangoFontDescription *s_font_bar = NULL;    /* parseada de bar->config.font */
static PangoFontDescription *s_font_power = NULL;  /* "sans bold 15" */

static cairo_surface_t *s_cs[2]   = { NULL, NULL };
static cairo_t         *s_cr[2]   = { NULL, NULL };
static PangoLayout     *s_lay[2]  = { NULL, NULL };
static PangoLayout     *s_lay_power[2] = { NULL, NULL };
static int s_cs_w = 0, s_cs_h = 0;

static cairo_pattern_t *s_grad_normal = NULL;
static cairo_pattern_t *s_grad_hover  = NULL;
static int s_grad_btn_h = 0;

static void ensure_cairo(struct BarState *bar, void *data) {
    int b = bar->cur_buf;
    if (s_cs[b] && s_cs_w == bar->buf_width && s_cs_h == bar->buf_height)
        return;

    if (s_cs_w != bar->buf_width || s_cs_h != bar->buf_height) {
        for (int i = 0; i < 2; i++) {
            if (s_lay[i]) { g_object_unref(s_lay[i]); s_lay[i] = NULL; }
            if (s_lay_power[i]) { g_object_unref(s_lay_power[i]); s_lay_power[i] = NULL; }
            if (s_cr[i])  { cairo_destroy(s_cr[i]); s_cr[i] = NULL; }
            if (s_cs[i])  { cairo_surface_destroy(s_cs[i]); s_cs[i] = NULL; }
        }
    }

    s_cs[b] = cairo_image_surface_create_for_data(
        (unsigned char *)data, CAIRO_FORMAT_ARGB32,
        bar->buf_width, bar->buf_height, bar->buf_stride);
    s_cr[b]  = cairo_create(s_cs[b]);

    s_lay[b] = pango_cairo_create_layout(s_cr[b]);
    if (!s_font_bar) s_font_bar = pango_font_description_from_string(bar->config.font);
    pango_layout_set_font_description(s_lay[b], s_font_bar);

    s_lay_power[b] = pango_cairo_create_layout(s_cr[b]);
    if (!s_font_power) s_font_power = pango_font_description_from_string("sans bold 15");
    pango_layout_set_font_description(s_lay_power[b], s_font_power);
    pango_layout_set_text(s_lay_power[b], "⏻", -1);

    s_cs_w = bar->buf_width; s_cs_h = bar->buf_height;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void set_col(cairo_t *cr, double r, double g, double b, double a) {
    cairo_set_source_rgba(cr, r, g, b, a);
}

/* Parse "#RRGGBB" or "#RRGGBBAA" into r,g,b,a in [0,1]. Returns false if invalid. */
static bool parse_hex_color(const char *hex, double *r, double *g, double *b, double *a) {
    if (!hex || hex[0] != '#') return false;
    size_t len = strlen(hex);
    unsigned int ri, gi, bi, ai = 255;
    if (len == 7) {
        if (sscanf(hex + 1, "%2x%2x%2x", &ri, &gi, &bi) != 3) return false;
    } else if (len == 9) {
        if (sscanf(hex + 1, "%2x%2x%2x%2x", &ri, &gi, &bi, &ai) != 4) return false;
    } else {
        return false;
    }
    *r = ri / 255.0; *g = gi / 255.0; *b = bi / 255.0; *a = ai / 255.0;
    return true;
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
    if (!s_font_bar) {
        s_font_bar = pango_font_description_from_string(font_desc);
    }
    pango_layout_set_font_description(lay, s_font_bar);
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
        bool hovered = (bar->hover_type == HIT_QL) && (bar->hover_index == i);

        /* Calculate button width */
        const char *glyph = bar_icon_nf_glyph(btn->icon);
        cairo_surface_t *icon = glyph ? NULL : bar_icon_get(btn->icon, ICON_SIZE);

        int btn_w;
        int glyph_tw = 0, glyph_th = 0;
        if (glyph) {
            pango_layout_set_text(lay, glyph, -1);
            pango_layout_get_pixel_size(lay, &glyph_tw, &glyph_th);
            btn_w = glyph_tw + BTN_PAD * 2;
        } else {
            btn_w = ICON_SIZE + BTN_PAD * 2;
        }
        if (btn->width >= 2)
            btn_w = btn_w * btn->width;

        /* Button background: custom color if set, flat hover highlight otherwise */
        {
            if (btn->has_bg) {
                cairo_set_source_rgba(cr, btn->bg_r, btn->bg_g, btn->bg_b, btn->bg_a);
                rounded_rect(cr, x, btn_y, btn_w, btn_h, BTN_RADIUS);
                cairo_fill(cr);
                if (hovered) {
                    /* white overlay for brightness boost */
                    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
                    rounded_rect(cr, x, btn_y, btn_w, btn_h, BTN_RADIUS);
                    cairo_fill(cr);
                }
            } else if (hovered) {
                cairo_set_source_rgba(cr, 0.30, 0.30, 0.38, 0.55);
                rounded_rect(cr, x, btn_y, btn_w, btn_h, BTN_RADIUS);
                cairo_fill(cr);
            }
        }

        /* Draw icon or glyph */
        if (glyph) {
            set_col(cr, COL_TEXT);
            cairo_move_to(cr, x + (btn_w - glyph_tw) / 2.0, btn_y + (btn_h - glyph_th) / 2.0);
            pango_cairo_show_layout(cr, lay);
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
        push_hit(HIT_QL, i, x, 0, btn_w, h);

        x += btn_w + 4;
        if (i == 0) x += 10; /* extra gap after start button */
    }

    return x; /* next x position */
}

/* ------------------------------------------------------------------ */
/* Draw taskbar section                                                  */
/* ------------------------------------------------------------------ */

static void draw_taskbar(cairo_t *cr, PangoLayout *lay, int x_start, int x_end, int h) {
    struct BarState *bar = &g_bar;
    int n = 0;
    for (int i = 0; i < bar->toplevel_n; i++) {
        if (!bar->toplevels[i].has_parent) n++;
    }
    if (n == 0) return;

    int avail  = x_end - x_start - 4;
    int max_btn_w = 160;
    int btn_w  = (n > 0) ? (avail / n) : 0;
    if (btn_w > max_btn_w) btn_w = max_btn_w;
    if (btn_w < 32) btn_w = 32;

    int btn_h = h - 4;
    int btn_y = 2;
    int x = x_start + 4;

    for (int i = 0; i < bar->toplevel_n; i++) {
        struct BarToplevel *tl = &bar->toplevels[i];
        if (tl->has_parent) continue;

        bool hovered = (bar->hover_type == HIT_TASKBAR) && (bar->hover_index == i);

        /* Always: gradient fill + 1px border (3D glass effect) */
        {
            double bx = x, by = btn_y;
            double bw = btn_w - 2, bh = btn_h;

            /* Rebuild static gradient patterns when btn_h changes (e.g. first frame or resize).
             * The gradient is purely vertical (x0==x1), so bx is irrelevant; use bx=0. */
            if (s_grad_btn_h != btn_h || !s_grad_normal) {
                if (s_grad_normal) cairo_pattern_destroy(s_grad_normal);
                if (s_grad_hover)  cairo_pattern_destroy(s_grad_hover);
                s_grad_normal = cairo_pattern_create_linear(0, 0, 0, bh);
                cairo_pattern_add_color_stop_rgba(s_grad_normal, 0.0, 0.22, 0.22, 0.30, 0.9);
                cairo_pattern_add_color_stop_rgba(s_grad_normal, 1.0, 0.13, 0.13, 0.17, 0.9);
                s_grad_hover = cairo_pattern_create_linear(0, 0, 0, bh);
                cairo_pattern_add_color_stop_rgba(s_grad_hover, 0.0, 0.32, 0.32, 0.42, 1.0);
                cairo_pattern_add_color_stop_rgba(s_grad_hover, 1.0, 0.18, 0.18, 0.24, 1.0);
                s_grad_btn_h = btn_h;
            }

            /* Translate the pattern to match the button's vertical position */
            cairo_matrix_t m;
            cairo_matrix_init_translate(&m, 0, -by);
            cairo_pattern_set_matrix(hovered ? s_grad_hover : s_grad_normal, &m);

            rounded_rect(cr, bx, by, bw, bh, BTN_RADIUS);
            cairo_set_source(cr, hovered ? s_grad_hover : s_grad_normal);
            cairo_fill(cr);

            /* 1px border: top-left lighter, bottom-right darker */
            rounded_rect(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, BTN_RADIUS);
            cairo_set_source_rgba(cr, 0.55, 0.55, 0.65, 0.5);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }

        /* Active: underline at the bottom */
        if (tl->activated) {
            cairo_set_source_rgba(cr, 0.30, 0.57, 1.0, 1.0);
            double lx = x + 3;
            double lw = btn_w - 7;
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

        push_hit(HIT_TASKBAR, i, x, 0, btn_w - 2, h);
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
    int slot_w = ICON_SIZE + BTN_PAD * 2; /* 34 pixels */
    int gap  = 2;
    int btn_y = (h - sz) / 2;

    struct BarState *bar = &g_bar;
    int btn_h = h - 4;
    int btn_y2 = 2;
    for (int i = n - 1; i >= 0; i--) {
        x -= slot_w + gap;
        bool hovered = (bar->hover_type == HIT_TRAY) && (bar->hover_index == i);
        if (hovered) {
            cairo_set_source_rgba(cr, 0.30, 0.30, 0.38, 0.55);
            rounded_rect(cr, x, btn_y2, slot_w, btn_h, BTN_RADIUS);
            cairo_fill(cr);
        }
        cairo_surface_t *icon = bar_tray_icon(i);
        int ix = x + (slot_w - sz) / 2;
        bar_icon_draw(cr, icon, ix, btn_y, sz);
        push_hit(HIT_TRAY, i, x, 0, slot_w, h);
    }
    return x;
}

/* Draw a battery icon via Cairo. cx,cy = center, w x h = bounding box */
static void draw_battery_icon(cairo_t *cr, double cx, double cy,
                               int level, bool charging) {
    double bw = 14.0, bh = 9.0;
    double bx = cx - bw / 2.0, by = cy - bh / 2.0;
    double tip_w = 3.0, tip_h = 4.0;

    /* Color: green ≥75, yellow ≥50, orange ≥25, red <25 */
    double r, g, b;
    if (charging)          { r=0.30; g=0.80; b=0.40; }
    else if (level >= 75)  { r=0.30; g=0.85; b=0.35; }
    else if (level >= 50)  { r=0.85; g=0.80; b=0.20; }
    else if (level >= 25)  { r=0.95; g=0.55; b=0.10; }
    else                   { r=0.90; g=0.20; b=0.20; }

    /* Outline */
    cairo_set_source_rgba(cr, 0.70, 0.70, 0.70, 0.9);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_stroke(cr);

    /* Terminal nub on right */
    cairo_rectangle(cr, bx + bw, by + (bh - tip_h) / 2.0, tip_w, tip_h);
    cairo_fill(cr);

    /* Fill bar (inset 1.5px) */
    double fill_w = (bw - 3.0) * (level > 100 ? 1.0 : level / 100.0);
    if (fill_w > 0) {
        cairo_set_source_rgba(cr, r, g, b, 0.95);
        cairo_rectangle(cr, bx + 1.5, by + 1.5, fill_w, bh - 3.0);
        cairo_fill(cr);
    }

    /* Charging bolt */
    if (charging) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 0.6, 1.0);
        cairo_set_line_width(cr, 1.2);
        cairo_move_to(cr, cx + 1.5, by + 1.0);
        cairo_line_to(cr, cx - 1.5, cy);
        cairo_line_to(cr, cx + 1.0, cy);
        cairo_line_to(cr, cx - 1.5, by + bh - 1.0);
        cairo_stroke(cr);
    }
}

/* Draw a speaker/volume icon via Cairo */
static void draw_volume_icon(cairo_t *cr, double cx, double cy, int vol, bool muted) {
    double sx = cx - 7.0, sy = cy;
    double spk_w = 5.0, spk_h = 7.0;

    cairo_set_line_width(cr, 1.2);

    if (muted) {
        cairo_set_source_rgba(cr, 0.65, 0.65, 0.65, 0.9);
    } else {
        cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, 1.0);
    }

    /* Speaker body (trapezoid) */
    cairo_move_to(cr, sx,            sy - 2.0);
    cairo_line_to(cr, sx + spk_w,    sy - spk_h / 2.0);
    cairo_line_to(cr, sx + spk_w,    sy + spk_h / 2.0);
    cairo_line_to(cr, sx,            sy + 2.0);
    cairo_close_path(cr);
    cairo_fill(cr);

    if (muted) {
        /* X mark */
        cairo_set_source_rgba(cr, 0.90, 0.25, 0.25, 1.0);
        cairo_set_line_width(cr, 1.4);
        double mx = sx + spk_w + 2.5;
        cairo_move_to(cr, mx,       sy - 2.5);
        cairo_line_to(cr, mx + 4.0, sy + 2.5);
        cairo_move_to(cr, mx + 4.0, sy - 2.5);
        cairo_line_to(cr, mx,       sy + 2.5);
        cairo_stroke(cr);
    } else {
        cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, 1.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        double ax = sx + spk_w + 2.5;
        if (vol > 0) {
            /* small arc */
            cairo_arc(cr, ax - 0.5, sy, 3.0, -M_PI * 0.45, M_PI * 0.45);
            cairo_stroke(cr);
        }
        if (vol >= 50) {
            /* medium arc */
            cairo_arc(cr, ax - 0.5, sy, 5.5, -M_PI * 0.45, M_PI * 0.45);
            cairo_stroke(cr);
        }
    }
}

static int draw_status(cairo_t *cr, PangoLayout *lay, int h, int x_end) {
    struct BarState *bar = &g_bar;
    int x = x_end;
    int btn_h = h - 4;
    int btn_y = 2;
    int icon_slot = ICON_SIZE + BTN_PAD * 2; /* fixed-width slot for icon modules */

    for (int i = bar->config.status_n - 1; i >= 0; i--) {
        const char *mod = bar->config.status[i];
        bool hovered = (bar->hover_type == HIT_STATUS) && (bar->hover_index == i);

        if (strcmp(mod, "power") == 0) {
            int pw = POWER_BTN_W;
            x -= pw + 2;
            if (hovered) {
                cairo_set_source_rgba(cr, 0.80, 0.15, 0.15, 0.25);
                rounded_rect(cr, x, btn_y, pw, btn_h, BTN_RADIUS);
                cairo_fill(cr);
            }
            int b = bar->cur_buf;
            PangoLayout *pw_lay = s_lay_power[b];
            if (pw_lay) {
                int tw, th;
                pango_layout_get_pixel_size(pw_lay, &tw, &th);
                cairo_set_source_rgba(cr, 1.0, 0.35, 0.35, 1.0);
                cairo_move_to(cr,
                              x + (pw - tw) / 2.0,
                              btn_y + (btn_h - th) / 2.0 + POWER_ICON_Y_OFFSET);
                pango_cairo_show_layout(cr, pw_lay);
            }
            push_hit(HIT_STATUS, i, x, 0, pw, h);

        } else if (strcmp(mod, "battery") == 0 && bar->bat_level >= 0) {
            x -= icon_slot + 2;
            if (hovered) {
                cairo_set_source_rgba(cr, 0.30, 0.30, 0.38, 0.55);
                rounded_rect(cr, x, btn_y, icon_slot, btn_h, BTN_RADIUS);
                cairo_fill(cr);
            }
            double cx = x + icon_slot / 2.0;
            double cy = btn_y + btn_h / 2.0;
            draw_battery_icon(cr, cx, cy, bar->bat_level, bar->bat_charging);
            push_hit(HIT_STATUS, i, x, 0, icon_slot, h);

        } else if (strcmp(mod, "volume") == 0 && bar->vol_text[0]) {
            x -= icon_slot + 2;
            if (hovered) {
                cairo_set_source_rgba(cr, 0.30, 0.30, 0.38, 0.55);
                rounded_rect(cr, x, btn_y, icon_slot, btn_h, BTN_RADIUS);
                cairo_fill(cr);
            }
            double cx = x + icon_slot / 2.0;
            double cy = btn_y + btn_h / 2.0;
            draw_volume_icon(cr, cx, cy, 50, false);
            push_hit(HIT_STATUS, i, x, 0, icon_slot, h);

        } else if (strcmp(mod, "clock") == 0 && bar->clock_text[0]) {
            pango_layout_set_text(lay, bar->clock_text, -1);
            int tw, th; (void)th;
            pango_layout_get_pixel_size(lay, &tw, &th);
            int btn_w = tw + BTN_PAD * 2;
            x -= btn_w + 2;
            if (hovered) {
                cairo_set_source_rgba(cr, 0.30, 0.30, 0.38, 0.55);
                rounded_rect(cr, x, btn_y, btn_w, btn_h, BTN_RADIUS);
                cairo_fill(cr);
            }
            set_col(cr, COL_TEXT);
            cairo_move_to(cr, x + (btn_w - tw) / 2.0, btn_y + (btn_h - th) / 2.0);
            pango_cairo_show_layout(cr, lay);
            push_hit(HIT_STATUS, i, x, 0, btn_w, h);
        }
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

    ensure_cairo(bar, data);
    int b = bar->cur_buf;
    cairo_t *cr = s_cr[b];
    PangoLayout *lay = s_lay[b];

    cairo_save(cr);

    /* Background */
    set_col(cr, COL_BG);
    cairo_paint(cr);

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

    cairo_restore(cr);

    bar_surface_commit();
    bar->dirty = false;
}

void bar_render_cleanup(void) {
    if (s_grad_normal) { cairo_pattern_destroy(s_grad_normal); s_grad_normal = NULL; }
    if (s_grad_hover)  { cairo_pattern_destroy(s_grad_hover);  s_grad_hover  = NULL; }
    s_grad_btn_h = 0;
    for (int i = 0; i < 2; i++) {
        if (s_lay[i]) { g_object_unref(s_lay[i]); s_lay[i] = NULL; }
        if (s_lay_power[i]) { g_object_unref(s_lay_power[i]); s_lay_power[i] = NULL; }
        if (s_cr[i])  { cairo_destroy(s_cr[i]); s_cr[i] = NULL; }
        if (s_cs[i])  { cairo_surface_destroy(s_cs[i]); s_cs[i] = NULL; }
    }
    if (s_font_bar) {
        pango_font_description_free(s_font_bar);
        s_font_bar = NULL;
    }
    if (s_font_power) {
        pango_font_description_free(s_font_power);
        s_font_power = NULL;
    }
}
