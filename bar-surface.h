#ifndef BAR_SURFACE_H
#define BAR_SURFACE_H

void bar_surface_create(void);
void bar_surface_destroy(void);
void bar_surface_restore(void);
void bar_surface_resize(int w, int h);
void bar_surface_commit(void);
void *bar_surface_get_draw_data(void);
void bg_surface_create(void);
void bg_surface_cleanup(void);

#endif /* BAR_SURFACE_H */
