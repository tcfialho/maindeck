#ifndef PROXY_STATE_H
#define PROXY_STATE_H

#include "proxy-types.h"

extern struct Client clients[MAX_CLIENTS];
extern pthread_mutex_t clients_mu;

extern bool opt_synthesize;
extern struct wl_display *proxy_display;

#endif /* PROXY_STATE_H */
