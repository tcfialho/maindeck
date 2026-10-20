#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bar-state.h"
#include "bar-render.h"

struct BarState g_bar;
static int latest;
static int tray_items = 1;
int bar_tray_count(void) { return tray_items; }
cairo_surface_t *bar_tray_icon(int i) { return NULL; }
void *bar_surface_get_draw_data(void) {
    char *data = (char *)g_bar.shm_data + g_bar.cur_buf * g_bar.buf_size;
    if (g_bar.prev_boxes_valid && !(g_bar.dirty_flags & BAR_DIRTY_ALL) && latest != g_bar.cur_buf)
        memcpy(data, (char *)g_bar.shm_data + latest * g_bar.buf_size, g_bar.buf_size);
    return data;
}
void bar_surface_commit(void) {
    latest = g_bar.cur_buf;
    g_bar.prev_boxes_valid = true;
    g_bar.dirty_flags = 0;
    g_bar.cur_buf ^= 1;
}
cairo_surface_t *bar_icon_get(const char *name, int size) { return NULL; }
const char *bar_icon_nf_glyph(const char *name) { return !strncmp(name,"nf:",3) ? name+3 : NULL; }
void bar_icon_draw(cairo_t *cr, cairo_surface_t *icon, double x, double y, int size) {
    cairo_set_source_rgb(cr, .4, .4, .4); cairo_rectangle(cr,x,y,size,size); cairo_fill(cr);
}
static void compare_partial(uint32_t flags) {
    g_bar.dirty_flags = flags;
    bar_render();
    unsigned char *partial = malloc(g_bar.buf_size);
    memcpy(partial, (char *)g_bar.shm_data + latest*g_bar.buf_size,g_bar.buf_size);
    g_bar.dirty_flags = BAR_DIRTY_ALL;
    bar_render();
    unsigned char *full = (unsigned char *)g_bar.shm_data + latest*g_bar.buf_size;
    if (memcmp(partial,full,g_bar.buf_size)) {
        for(int i=0;i<g_bar.buf_size;i++) if(partial[i]!=full[i]) {
            fprintf(stderr,"pixel mismatch flags=%u x=%d y=%d partial=%u full=%u\n",flags,(i/4)%g_bar.buf_width,(i/4)/g_bar.buf_width,partial[i],full[i]); break;
        }
        abort();
    }
    free(partial);
}
int main(void) {
    g_bar.configured = true;
    g_bar.buf_width=1280; g_bar.buf_height=32; g_bar.buf_stride=1280*4; g_bar.buf_size=1280*32*4;
    g_bar.shm_data=calloc(2,g_bar.buf_size);
    strcpy(g_bar.config.font,"Sans 10");
    g_bar.config.ql_count=2;
    strcpy(g_bar.config.ql[0].icon,"nf:S"); strcpy(g_bar.config.ql[1].icon,"nf:T");
    g_bar.config.status_n=3;
    g_bar.config.status[0]=BAR_STATUS_BATTERY;
    g_bar.config.status[1]=BAR_STATUS_CLOCK;
    g_bar.config.status[2]=BAR_STATUS_POWER;
    strcpy(g_bar.clock_text,"15:42"); g_bar.bat_level=50;
    for(int i=0;i<3;i++) {
        g_bar.toplevels[i]=calloc(1,sizeof(struct BarToplevel));
        snprintf(g_bar.toplevels[i]->title,256,"Window %d",i);
    }
    g_bar.toplevel_n=3;
    g_bar.dirty_flags=BAR_DIRTY_ALL; bar_render();
    for(int i=0;i<100;i++) {
        g_bar.hover_type=HIT_TASKBAR; g_bar.hover_index=i%3;
        g_bar.toplevels[1]->minimized=i%2;
        compare_partial(BAR_DIRTY_TASKBAR|BAR_DIRTY_QUICKLAUNCH);
        g_bar.hover_type=HIT_STATUS; g_bar.hover_index=i%3;
        g_bar.bat_level=i;
        compare_partial(BAR_DIRTY_TASKBAR|BAR_DIRTY_STATUS);
        g_bar.hover_type=HIT_QL; g_bar.hover_index=i%2;
        compare_partial(BAR_DIRTY_STATUS|BAR_DIRTY_QUICKLAUNCH);
        tray_items=i%4;
        compare_partial(BAR_DIRTY_TRAY);
    }
    strcpy(g_bar.clock_text,"Wednesday, 15:43");
    compare_partial(BAR_DIRTY_STATUS);
    for (int i=0;i<100;i++) {
        snprintf(g_bar.toplevels[0]->title,256,"Updated title %d",i);
        compare_partial(BAR_DIRTY_TASKBAR);
    }
    g_bar.hover_type=HIT_TASKBAR; g_bar.hover_index=0;
    g_bar.dirty_flags=BAR_DIRTY_ALL; bar_render();
    double elapsed[2];
    for (int mode=0;mode<2;mode++) {
        struct timespec a,b;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&a);
        for (int i=0;i<3000;i++) {
            g_bar.hover_type=HIT_TASKBAR; g_bar.hover_index=i%3;
            g_bar.dirty_flags=mode ? BAR_DIRTY_TASKBAR : BAR_DIRTY_ALL;
            bar_render();
        }
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&b);
        elapsed[mode]=(b.tv_sec-a.tv_sec)*1000.0+(b.tv_nsec-a.tv_nsec)/1e6;
    }
    printf("Render CPU, 3000 hover frames: full=%.2f ms regional=%.2f ms\n",elapsed[0],elapsed[1]);
    bar_render_cleanup();
    free(g_bar.shm_data);
    puts("PASS: 501 partial redraws match full frames byte for byte");
}
