#ifndef PROXY_RELAY_H
#define PROXY_RELAY_H

#include "proxy-types.h"

void setup_client(int waybar_fd);
bool connect_to_river(int *fd_out);

#endif /* PROXY_RELAY_H */
