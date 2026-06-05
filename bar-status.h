#ifndef BAR_STATUS_H
#define BAR_STATUS_H

/* Init clock/battery. vol_text set to static placeholder for pavucontrol launcher.
 * Returns -1 (no pollable fd). */
int  bar_status_init(void);

/* Call once per minute from the main timer tick. Updates clock + battery. */
void bar_status_tick(void);

void bar_status_cleanup(void);
void bar_update_volume(const char *text);

#endif /* BAR_STATUS_H */
