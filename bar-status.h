#ifndef BAR_STATUS_H
#define BAR_STATUS_H

/* Init pulse subscription and start battery/clock polling.
 * Returns fd to add to poll set (pulse mainloop fd), or -1 on failure. */
int  bar_status_init(void);

/* Drain pending pulse events (call when pulse fd is readable). */
void bar_status_pulse_dispatch(void);

/* Call once per minute from the main timer tick. Updates clock + battery. */
void bar_status_tick(void);

void bar_status_cleanup(void);

#endif /* BAR_STATUS_H */
