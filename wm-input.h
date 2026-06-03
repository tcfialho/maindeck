#ifndef WM_INPUT_H
#define WM_INPUT_H

#include "types.h"

void close_launcher(void);
int compute_poll_timeout(void);
void process_hold_timers(void);
void seat_maybe_destroy(struct Seat *seat);
void seat_manage(struct Seat *seat);

extern const struct river_seat_v1_listener river_seat_listener;

#endif /* WM_INPUT_H */
