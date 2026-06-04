#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include "proxy-log.h"
#include "proxy-wire.h"


uint32_t ru32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

void wu32(uint8_t *p, uint32_t v) {
    p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}

bool message_has_fds(struct msghdr *mh) {
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(mh);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(mh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            return true;
        }
    }
    return false;
}

/* Close all FDs received via SCM_RIGHTS in a recvmsg cmsgbuf.
 * Must be called after sendmsg() to release the kernel-duplicated copies. */
void close_received_fds(struct msghdr *mh) {
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(mh);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(mh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fds = (int *)CMSG_DATA(cmsg);
            for (size_t i = 0; i < nfds; i++) {
                if (fds[i] >= 0) close(fds[i]);
            }
        }
    }
}

/* Relay one chunk of data + ancillary fds from src to dst. */
ssize_t relay_chunk(int src, int dst) {
    uint8_t buf[BUF_SZ];
    /* space for up to 28 fds (max typical in one recvmsg) */
    uint8_t cmsgbuf[CMSG_SPACE(28 * sizeof(int))];

    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr mh = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsgbuf,
        .msg_controllen = sizeof(cmsgbuf),
    };
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    ssize_t n = recvmsg(src, &mh, 0);
    if (n <= 0) return n;

    /* forward data + ancillary (fds) verbatim */
    iov.iov_len = (size_t)n;
    mh.msg_flags = 0;
    if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) {
        close_received_fds(&mh);
        return -1;
    }
    close_received_fds(&mh);
    return n;
}

/* Write a synthetic Wayland event directly to a client fd (no fds needed). */
ssize_t raw_write(int fd, pthread_mutex_t *mu,
                         const uint8_t *buf, size_t len) {
    struct iovec iov = { .iov_base = (void*)buf, .iov_len = len };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t r;
    pthread_mutex_lock(mu);
    r = sendmsg(fd, &mh, MSG_NOSIGNAL);
    pthread_mutex_unlock(mu);
    if (r < 0) {
        plog_err("raw_write: sendmsg fd=%d len=%zu errno=%d (%s)",
             fd, len, errno, strerror(errno));
    } else if ((size_t)r != len) {
        plog("raw_write: short write fd=%d wrote=%zd want=%zu",
             fd, r, len);
    }
    return r;
}

/* Encode a Wayland string (length-prefixed, NUL-terminated, 4-byte padded). */
int encode_str(uint8_t *out, size_t cap, const char *s) {
    if (!s) s = "";
    uint32_t slen = (uint32_t)strlen(s) + 1;
    uint32_t pad  = (slen + 3u) & ~3u;
    if (4u + pad > cap) return -1;
    wu32(out, slen);
    memcpy(out+4, s, slen);
    memset(out+4+slen, 0, pad-slen);
    return (int)(4u + pad);
}
