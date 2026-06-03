#ifndef WM_LIBINPUT_H
#define WM_LIBINPUT_H

#include "types.h"

void init_libinput_listeners(void);

extern const struct river_libinput_config_v1_listener libinput_config_listener;

#endif /* WM_LIBINPUT_H */
