#ifndef PROXY_WIRE_H
#define PROXY_WIRE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <pthread.h>

#define MSG_HDR 8u   /* object_id(4) + size_opcode(4) */
#define BUF_SZ  65536u

uint32_t ru32(const uint8_t *p);
void wu32(uint8_t *p, uint32_t v);
bool message_has_fds(struct msghdr *mh);
void close_received_fds(struct msghdr *mh);
ssize_t relay_chunk(int src, int dst);
ssize_t raw_write(int fd, pthread_mutex_t *mu, const uint8_t *buf, size_t len);
int encode_str(uint8_t *out, size_t cap, const char *s);

#endif /* PROXY_WIRE_H */
