#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "proxy-types.h"
#include "proxy-log.h"
#include "proxy-state.h"
#include "proxy-wire.h"
#include "proxy-emit.h"
#include "proxy-toplevel.h"
#include "proxy-relay.h"

struct Client    clients[MAX_CLIENTS];
pthread_mutex_t  clients_mu = PTHREAD_MUTEX_INITIALIZER;

static void client_thread_exit(struct Client *c);

static struct Client *alloc_client(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].active && clients[i].active_threads == 0) return &clients[i];
    return NULL;
}

static int real_handle_index(struct Client *c, uint32_t hid) {
    for (int i = 0; i < c->real_handle_n; i++)
        if (c->real_handle_ids[i] == hid) return i;
    return -1;
}

static void inject_output_enter_for(struct Client *c, uint32_t hid) {
    int i = real_handle_index(c, hid);
    if (i < 0) return;
    if (c->real_handle_entered[i]) return;
    if (!c->output_id) return;
    emit_output_enter(c, hid);
    c->real_handle_entered[i] = true;
    plog("dirB: injected output_enter(handle=%u, output=%u)", hid, c->output_id);
}

static void real_handle_add(struct Client *c, uint32_t hid) {
    if (real_handle_index(c, hid) >= 0) return;
    if (c->real_handle_n >= HANDLE_MAP) return;
    c->real_handle_ids[c->real_handle_n] = hid;
    c->real_handle_entered[c->real_handle_n] = false;
    c->real_handle_n++;
}

static void real_handle_remove(struct Client *c, uint32_t hid) {
    int i = real_handle_index(c, hid);
    if (i < 0) return;
    for (int j = i; j < c->real_handle_n - 1; j++) {
        c->real_handle_ids[j] = c->real_handle_ids[j+1];
        c->real_handle_entered[j] = c->real_handle_entered[j+1];
    }
    c->real_handle_n--;
}

static bool real_handle_identifier(struct Client *c, uint32_t hid, char out[33]) {
    int i = real_handle_index(c, hid);
    if (i < 0) return false;
    bool ok = false;
    pthread_mutex_lock(&state_mu);
    int j = 0;
    struct Toplevel *tt;
    wl_list_for_each(tt, &toplevels, link) {
        if (j == i) {
            if (tt->ext != NULL && tt->ext->identifier[0] != '\0') {
                snprintf(out, 33, "%s", tt->ext->identifier);
                ok = true;
            }
            break;
        }
        j++;
    }
    pthread_mutex_unlock(&state_mu);
    return ok;
}

static bool send_wm_activate(const char *identifier) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !identifier || identifier[0] == '\0') return false;

    char path[256];
    if ((size_t)snprintf(path, sizeof(path), "%s/maindeck-wm.sock", dir) >= sizeof(path))
        return false;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    char msg[64];
    int len = snprintf(msg, sizeof(msg), "activate %s", identifier);
    if (len < 0 || (size_t)len >= sizeof(msg)) {
        close(fd);
        return false;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) >= sizeof(addr.sun_path)) {
        close(fd);
        return false;
    }

    bool ok = sendto(fd, msg, (size_t)len, MSG_NOSIGNAL,
        (struct sockaddr *)&addr, sizeof(addr)) == (ssize_t)len;
    if (!ok) {
        plog_err("send_wm_activate: sendto(%s) failed errno=%d (%s)",
             path, errno, strerror(errno));
    }
    close(fd);
    return ok;
}

static bool handle_taskbar_activate(struct Client *c, uint32_t obj_id, uint32_t opcode) {
    if (opcode != 4) return false;
    if (real_handle_index(c, obj_id) < 0) return false;

    char identifier[33];
    identifier[0] = '\0';
    bool got_id = real_handle_identifier(c, obj_id, identifier);
    if (!got_id || identifier[0] == '\0') {
        plog("taskbar activate: handle=%u sem identifier (ext ainda não chegou?), ignorando", obj_id);
        return true;
    }

    bool sent = send_wm_activate(identifier);
    plog("taskbar activate: handle=%u identifier=%s enviado=%s",
         obj_id, identifier, sent ? "sim" : "FALHOU");
    return true;
}

static void replay_real_output_enters(struct Client *c) {
    if (!c->output_id) return;
    for (int i = 0; i < c->real_handle_n; i++)
        if (!c->real_handle_entered[i]) {
            emit_output_enter(c, c->real_handle_ids[i]);
            c->real_handle_entered[i] = true;
            plog("dirB: replayed output_enter(handle=%u, output=%u)",
                 c->real_handle_ids[i], c->output_id);
        }
}

static void *relay_c2s(void *arg) {
    struct RelayArgs *ra = arg;
    struct Client *c = ra->c;
    int src = ra->src_fd;
    int dst = ra->dst_fd;
    free(ra);
    plog("relay_c2s: started src=%d dst=%d", src, dst);

    uint8_t  buf[BUF_SZ];
    uint8_t  cmsgbuf[CMSG_SPACE(28*sizeof(int))];
    size_t   pending = 0;

    while (1) {
        struct iovec iov = {
            .iov_base = buf + pending,
            .iov_len  = sizeof(buf) - pending,
        };
        struct msghdr mh = {
            .msg_iov        = &iov,
            .msg_iovlen     = 1,
            .msg_control    = cmsgbuf,
            .msg_controllen = sizeof(cmsgbuf),
        };
        memset(cmsgbuf, 0, sizeof(cmsgbuf));

        if (sizeof(buf) - pending == 0) {
            relay_chunk(src, dst);
            continue;
        }

        ssize_t n = recvmsg(src, &mh, 0);
        if (n <= 0) break;

        bool has_anc = message_has_fds(&mh);
        if (has_anc) {
            if (pending > 0) {
                if (send(dst, buf, pending, MSG_NOSIGNAL) < 0) goto done;
                memmove(buf, buf + pending, (size_t)n);
                pending = 0;
            }
            if (pending == 0) {
                uint8_t outbuf[BUF_SZ];
                size_t outn = 0;
                size_t scan = 0;
                bool complete = true;
                bool flush_after_send = false;
                uint32_t fake_xdg_ids[8];
                int fake_xdg_n = 0;
                while (scan + MSG_HDR <= (size_t)n) {
                    uint32_t sizeop = ru32(buf+scan+4);
                    uint32_t msize = sizeop >> 16;
                    if (msize < MSG_HDR || scan + msize > (size_t)n) {
                        complete = false;
                        break;
                    }
                    uint8_t *msg = buf + scan;
                    uint32_t obj_id = ru32(msg);
                    uint32_t opcode = sizeop & 0xffffu;
                    bool drop_msg = false;
                    uint8_t rewritten[256];
                    size_t rewritten_len = 0;

                    if (obj_id == 1 && opcode == 1 && msize >= 12) {
                        client_add_registry_id(c, ru32(msg+8));
                    }
                    if (client_is_registry_id(c, obj_id) && opcode == 0) {
                        uint32_t gname, gver, new_id;
                        char iface[128];
                        if (parse_bind(msg, msize, &gname, iface, sizeof(iface), &gver, &new_id)) {
                            if (!strcmp(iface, "zwlr_foreign_toplevel_manager_v1")) {
                                if (c->manager_n < 8) {
                                    c->manager_ids[c->manager_n++] = new_id;
                                }
                                flush_after_send = true;
                            } else if (!strcmp(iface, "wl_compositor")) {
                                c->compositor_id = new_id;
                            } else if (!strcmp(iface, "zxdg_output_manager_v1")) {
                                c->zxdg_output_manager_id = new_id;
                                c->zxdg_output_manager_version = gver;
                            } else if (!strcmp(iface, "zwlr_layer_shell_v1")) {
                                c->zwlr_layer_shell_id = new_id;
                            } else if (!strcmp(iface, "wl_data_device_manager")) {
                                c->data_device_manager_id = new_id;
                            } else if (!strcmp(iface, "wl_output") && !c->output_id) {
                                c->output_id = new_id;
                                c->output_version = gver;
                                c->output_is_fake = (gname == FAKE_OUTPUT_GLOBAL);
                                if (c->output_is_fake && c->fixes_global) {
                                    uint32_t fver = c->fixes_version < gver ? c->fixes_version : gver;
                                    if (fver == 0) fver = 1;
                                    rewritten_len = build_bind(rewritten, sizeof(rewritten), obj_id,
                                        c->fixes_global, "wl_fixes", fver, new_id);
                                }
                            } else {
                                bool already = false;
                                for (int u = 0; u < c->unhandled_n; u++) {
                                    if (!strcmp(c->unhandled_binds[u], iface)) { already = true; break; }
                                }
                                if (!already && c->unhandled_n < 32) {
                                    snprintf(c->unhandled_binds[c->unhandled_n],
                                        sizeof(c->unhandled_binds[0]), "%s", iface);
                                    c->unhandled_n++;
                                    plog("relay_c2s: unhandled bind iface='%s' version=%u new_id=%u",
                                         iface, gver, new_id);
                                }
                            }
                        }
                    }

                    if (!drop_msg && c->output_is_fake && obj_id == c->output_id) {
                        drop_msg = true;
                    }
                    if (!drop_msg && c->output_is_fake && c->zxdg_output_manager_id &&
                            obj_id == c->zxdg_output_manager_id && opcode == 1 && msize >= 16) {
                        uint32_t xdg_output_id = ru32(msg+8);
                        uint32_t output_id = ru32(msg+12);
                        if (output_id == c->output_id) {
                            if (c->compositor_id) {
                                wu32(rewritten+0, c->compositor_id);
                                wu32(rewritten+4, (12u<<16)|1u);
                                wu32(rewritten+8, xdg_output_id);
                                rewritten_len = 12;
                            } else {
                                drop_msg = true;
                            }
                            if (fake_xdg_n < 8) fake_xdg_ids[fake_xdg_n++] = xdg_output_id;
                        }
                    }
                    if (!drop_msg && c->output_is_fake && c->zwlr_layer_shell_id &&
                            obj_id == c->zwlr_layer_shell_id && opcode == 0 && msize >= 24) {
                        uint32_t ls_new_id = ru32(msg + 8);
                        client_add_layer_surface_id(c, ls_new_id);
                        plog("relay_c2s(anc): get_layer_surface new_id=%u tracked", ls_new_id);
                        if (c->output_is_fake) {
                            if (msize <= sizeof(rewritten)) {
                                memcpy(rewritten, msg, msize);
                                if (ru32(rewritten + 16) == c->output_id) {
                                    wu32(rewritten + 16, 0);
                                    rewritten_len = msize;
                                }
                            }
                        }
                    }

                    if (!drop_msg && c->manager_n > 0 &&
                            (client_is_manager_id(c, obj_id) || client_is_synthetic_handle_id(c, obj_id))) {
                        drop_msg = true;
                    }

                    const uint8_t *src_msg = rewritten_len ? rewritten : msg;
                    size_t src_len = rewritten_len ? rewritten_len : msize;
                    if (!drop_msg) {
                        if (outn + src_len > sizeof(outbuf)) {
                            complete = false;
                            break;
                        }
                        memcpy(outbuf + outn, src_msg, src_len);
                        outn += src_len;
                    }
                    scan += msize;
                }
                if (complete && scan == (size_t)n) {
                    struct iovec oiov = { .iov_base = outbuf, .iov_len = outn };
                    mh.msg_iov = &oiov;
                    mh.msg_iovlen = 1;
                    mh.msg_flags = 0;
                    if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) { close_received_fds(&mh); goto done; }
                    close_received_fds(&mh);
                    if (c->output_is_fake) emit_fake_output_events(c);
                    if (flush_after_send) {
                        pthread_mutex_lock(&clients_mu);
                        pthread_mutex_lock(&state_mu);
                        struct Toplevel *t;
                        wl_list_for_each(t, &toplevels, link)
                            flush_toplevel(c, t);
                        pthread_mutex_unlock(&state_mu);
                        pthread_mutex_unlock(&clients_mu);
                    }

                    for (int i = 0; i < fake_xdg_n; i++)
                        emit_fake_xdg_output_events(c, fake_xdg_ids[i]);
                    continue;
                }
            }
            iov.iov_base = buf + pending;
            iov.iov_len  = (size_t)n;
            mh.msg_flags = 0;
            if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) { close_received_fds(&mh); goto done; }
            close_received_fds(&mh);
            continue;
        }

        pending += (size_t)n;

        size_t off = 0;
        while (off + MSG_HDR <= pending) {
            uint32_t sizeop = ru32(buf+off+4);
            uint32_t msize  = sizeop >> 16;
            if (msize < MSG_HDR || msize > BUF_SZ) break;
            if (off + msize > pending) break;

            const uint8_t *msg    = buf + off;
            uint32_t       obj_id = ru32(msg);
            uint32_t       opcode = sizeop & 0xffffu;
            bool           drop   = false;
            bool           flush_after_forward = false;

            if (obj_id == 1 && opcode == 1 && msize >= 12) {
                client_add_registry_id(c, ru32(msg+8));
            }

            if (client_is_registry_id(c, obj_id) && opcode == 0) {
                uint32_t gname, gver, new_id;
                char iface[128];
                if (parse_bind(msg, msize, &gname, iface, sizeof(iface), &gver, &new_id)) {
                    if (!strcmp(iface, "zwlr_foreign_toplevel_manager_v1")) {
                        pthread_mutex_lock(&clients_mu);
                        if (c->manager_n < 8) {
                            c->manager_ids[c->manager_n++] = new_id;
                        }
                        pthread_mutex_unlock(&clients_mu);
                        flush_after_forward = true;
                    } else if (!strcmp(iface, "wl_compositor")) {
                        c->compositor_id = new_id;
                    } else if (!strcmp(iface, "zxdg_output_manager_v1")) {
                        c->zxdg_output_manager_id = new_id;
                        c->zxdg_output_manager_version = gver;
                    } else if (!strcmp(iface, "zwlr_layer_shell_v1")) {
                        c->zwlr_layer_shell_id = new_id;
                    } else if (!strcmp(iface, "wl_data_device_manager")) {
                        c->data_device_manager_id = new_id;
                    } else if (!strcmp(iface, "wl_output") && !c->output_id) {
                        c->output_id = new_id;
                        c->output_version = gver;
                        c->output_is_fake = (gname == FAKE_OUTPUT_GLOBAL);
                        if (c->output_is_fake) {
                            uint8_t rewritten[256];
                            uint32_t ver = c->compositor_version < 6 ? c->compositor_version : 6;
                            size_t len = build_bind(rewritten, sizeof(rewritten), obj_id,
                                c->compositor_global, "wl_compositor", ver, new_id);
                            if (len > 0 && c->compositor_global) {
                                if (send(dst, rewritten, len, MSG_NOSIGNAL) < 0) goto done;
                            }
                            emit_fake_output_events(c);
                            drop = true;
                        }
                    }
                }
            }

            if (!drop && c->output_is_fake && obj_id == c->output_id)
                drop = true;

            if (!drop && c->output_is_fake && c->zxdg_output_manager_id &&
                    obj_id == c->zxdg_output_manager_id && opcode == 1 && msize >= 16) {
                uint32_t xdg_output_id = ru32(msg+8);
                uint32_t output_id = ru32(msg+12);
                if (output_id == c->output_id) {
                    if (c->compositor_id) {
                        uint8_t create_region[12];
                        wu32(create_region+0, c->compositor_id);
                        wu32(create_region+4, (12u<<16)|1u);
                        wu32(create_region+8, xdg_output_id);
                        if (send(dst, create_region, sizeof(create_region), MSG_NOSIGNAL) < 0) goto done;
                    }
                    emit_fake_xdg_output_events(c, xdg_output_id);
                    drop = true;
                }
            }

            if (!drop && c->zwlr_layer_shell_id &&
                    obj_id == c->zwlr_layer_shell_id && opcode == 0 && msize >= 24) {
                uint32_t ls_new_id = ru32(msg + 8);
                client_add_layer_surface_id(c, ls_new_id);
                plog("relay_c2s: get_layer_surface new_id=%u tracked", ls_new_id);
                if (c->output_is_fake) {
                    uint8_t rewritten[256];
                    if (msize <= sizeof(rewritten)) {
                        memcpy(rewritten, msg, msize);
                        if (ru32(rewritten + 16) == c->output_id) {
                            wu32(rewritten + 16, 0);
                            if (send(dst, rewritten, msize, MSG_NOSIGNAL) < 0) goto done;
                            drop = true;
                        }
                    }
                }
            }

            if (!drop && c->manager_n > 0 &&
                    (client_is_manager_id(c, obj_id) || client_is_synthetic_handle_id(c, obj_id))) {
                drop = true;
            }

            if (!drop && handle_taskbar_activate(c, obj_id, opcode)) {
                drop = true;
            }

            if (!drop) {
                if (send(dst, msg, msize, MSG_NOSIGNAL) < 0) goto done;
            }
            if (flush_after_forward) {
                pthread_mutex_lock(&clients_mu);
                pthread_mutex_lock(&state_mu);
                struct Toplevel *t;
                wl_list_for_each(t, &toplevels, link)
                    flush_toplevel(c, t);
                pthread_mutex_unlock(&state_mu);
                pthread_mutex_unlock(&clients_mu);
            }

            off += msize;
        }

        if (off > 0) {
            pending -= off;
            if (pending) memmove(buf, buf+off, pending);
        }
    }
done:
    plog("relay_c2s: ending src=%d dst=%d errno=%d", src, dst, errno);
    client_thread_exit(c);
    return NULL;
}

static void *relay_s2c(void *arg) {
    struct RelayArgs *ra = arg;
    struct Client *c = ra->c;
    int src = ra->src_fd;
    int dst = ra->dst_fd;
    free(ra);
    plog("relay_s2c: started src=%d dst=%d", src, dst);

    uint8_t  buf[BUF_SZ];
    uint8_t  cmsgbuf[CMSG_SPACE(28*sizeof(int))];
    size_t   pending = 0;

    while (1) {
        struct iovec iov = {
            .iov_base = buf + pending,
            .iov_len  = sizeof(buf) - pending,
        };
        struct msghdr mh = {
            .msg_iov        = &iov,
            .msg_iovlen     = 1,
            .msg_control    = cmsgbuf,
            .msg_controllen = sizeof(cmsgbuf),
        };
        memset(cmsgbuf, 0, sizeof(cmsgbuf));

        if (sizeof(buf) - pending == 0) {
            pthread_mutex_lock(&c->write_mu);
            relay_chunk(src, dst);
            pthread_mutex_unlock(&c->write_mu);
            continue;
        }

        ssize_t n = recvmsg(src, &mh, 0);
        if (n <= 0) {
            plog("relay_s2c: recvmsg returned %zd errno=%d pending=%zu", n, errno, pending);
            break;
        }

        bool has_anc = message_has_fds(&mh);
        if (has_anc) {
            pthread_mutex_lock(&c->write_mu);
            if (pending > 0) {
                if (send(dst, buf, pending, MSG_NOSIGNAL) < 0) {
                    pthread_mutex_unlock(&c->write_mu);
                    plog_err("relay_s2c: send(pending) failed errno=%d", errno);
                    break;
                }
            }
            iov.iov_base = buf + pending;
            iov.iov_len  = (size_t)n;
            mh.msg_flags = 0;
            if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) {
                pthread_mutex_unlock(&c->write_mu);
                plog_err("relay_s2c: sendmsg(anc) failed errno=%d", errno);
                close_received_fds(&mh);
                break;
            }
            close_received_fds(&mh);
            pending = 0;
            pthread_mutex_unlock(&c->write_mu);
            continue;
        }

        if (pending == 0 && n >= 8) {
            size_t scan = 0;
            while (scan + 8 <= (size_t)n) {
                uint32_t obj = ru32(buf+scan);
                uint32_t so = ru32(buf+scan+4);
                uint32_t sz = so >> 16;
                uint32_t op = so & 0xffff;
                if (sz < 8 || scan + sz > (size_t)n) {
                    plog_err("relay_s2c: !!! malformed msg at off=%zu obj=%u op=%u size=%u chunk=%zd",
                         scan, obj, op, sz, n);
                    break;
                }
                if (obj == 1 && op == 0 && sz >= 16) {
                    uint32_t target = ru32(buf+scan+8);
                    uint32_t code = ru32(buf+scan+12);
                    uint32_t slen = ru32(buf+scan+16);
                    const char *s = (const char *)(buf+scan+20);
                    plog_err("relay_s2c: *** wl_display.error target=%u code=%u msg='%.*s'",
                         target, code, (int)slen, s);
                } else if (obj >= SERVER_ID_BASE && obj < c->next_sid &&
                           !client_should_drop_server_id(c, obj)) {
                    plog_err("relay_s2c: !!! server-ID collision: River obj=0x%x op=%u falls in proxy synthetic range [0x%x..0x%x)",
                         obj, op, SERVER_ID_BASE, c->next_sid);
                }
                scan += sz;
            }
        }

        pending += (size_t)n;

        size_t off = 0;
        while (off + MSG_HDR <= pending) {
            uint32_t sizeop = ru32(buf+off+4);
            uint32_t msize  = sizeop >> 16;
            if (msize < MSG_HDR || msize > BUF_SZ) break;
            if (off + msize > pending) break;

            const uint8_t *msg = buf + off;
            uint32_t obj_id = ru32(msg);
            uint32_t opcode = sizeop & 0xffffu;
            bool drop = false;

            pthread_mutex_lock(&clients_mu);
            if (client_is_registry_id(c, obj_id) && opcode == 0) {
                uint32_t gname, gver;
                char iface[128];
                if (parse_global(msg, msize, &gname, iface, sizeof(iface), &gver)) {
                    if (!strcmp(iface, "wl_compositor")) {
                        c->compositor_global = gname;
                        c->compositor_version = gver;
                        if (!c->fake_output_registry_id) c->fake_output_registry_id = obj_id;
                    } else if (!strcmp(iface, "wl_fixes")) {
                        c->fixes_global = gname;
                        c->fixes_version = gver;
                    }
                }
            }
            uint32_t new_real_handle = 0;
            bool     handle_closed   = false;
            if (c->manager_n > 0 && client_is_manager_id(c, obj_id) && opcode == 0 && msize >= 12) {
                uint32_t hid = ru32(msg+8);
                if (opt_synthesize) {
                    client_add_drop_server_id(c, hid);
                    drop = true;
                } else {
                    real_handle_add(c, hid);
                    new_real_handle = hid;
                }
            } else if (client_should_drop_server_id(c, obj_id)) {
                drop = true;
            } else if (!opt_synthesize && opcode == 6 && real_handle_index(c, obj_id) >= 0) {
                handle_closed = true;
            }
            pthread_mutex_unlock(&clients_mu);

            if (!drop) {
                pthread_mutex_lock(&c->write_mu);
                if (send(dst, msg, msize, MSG_NOSIGNAL) < 0) {
                    pthread_mutex_unlock(&c->write_mu);
                    plog_err("relay_s2c: send(dst=%d) failed obj=%u op=%u size=%u errno=%d (%s)",
                         dst, obj_id, opcode, msize, errno, strerror(errno));
                    goto done_s2c;
                }
                pthread_mutex_unlock(&c->write_mu);
            }

            if (new_real_handle) {
                pthread_mutex_lock(&clients_mu);
                inject_output_enter_for(c, new_real_handle);
                pthread_mutex_unlock(&clients_mu);
            }
            if (handle_closed) {
                pthread_mutex_lock(&clients_mu);
                real_handle_remove(c, obj_id);
                pthread_mutex_unlock(&clients_mu);
            }

            bool is_output_done = false;
            pthread_mutex_lock(&clients_mu);
            if (c->output_id && obj_id == c->output_id && opcode == 2) {
                is_output_done = true;
            }
            pthread_mutex_unlock(&clients_mu);

            if (is_output_done) {
                plog("relay_s2c: wl_output.done received for output_id=%u, replaying output enters", c->output_id);
                pthread_mutex_lock(&clients_mu);
                c->output_done_received = true;
                replay_output_enters(c);
                replay_real_output_enters(c);
                pthread_mutex_unlock(&clients_mu);
            }

            off += msize;
        }

        if (off > 0) {
            pending -= off;
            if (pending) memmove(buf, buf+off, pending);
        }
    }
done_s2c:
    plog("relay_s2c: ending src=%d dst=%d errno=%d", src, dst, errno);
    client_thread_exit(c);
    return NULL;
}

static void client_thread_exit(struct Client *c) {
    pthread_mutex_lock(&clients_mu);
    c->active_threads--;
    int threads = c->active_threads;
    if (threads == 1) {
        plog("client_thread_exit: primeira thread terminando, efetuando shutdown dos sockets");
        if (c->waybar_fd >= 0) shutdown(c->waybar_fd, SHUT_RDWR);
        if (c->river_fd >= 0) shutdown(c->river_fd, SHUT_RDWR);
    } else if (threads == 0) {
        plog("client_thread_exit: segunda thread terminando, liberando recursos do cliente");
        if (c->waybar_fd >= 0) {
            close(c->waybar_fd);
            c->waybar_fd = -1;
        }
        if (c->river_fd >= 0) {
            close(c->river_fd);
            c->river_fd = -1;
        }
        c->active = false;
        c->manager_n = 0;
        c->hmap_n = 0;
        c->drop_n = 0;
        c->layer_surface_n = 0;
        c->registry_n = 0;
        c->unhandled_n = 0;
        c->real_handle_n = 0;
        c->output_id = 0;
        c->data_device_manager_id = 0;
        c->output_done_received = false;
    }
    pthread_mutex_unlock(&clients_mu);
}

void setup_client(int waybar_fd) {
    plog("setup_client: waybar_fd=%d, connecting to River", waybar_fd);
    int river_fd;
    if (!connect_to_river(&river_fd)) {
        plog_err("setup_client: connect_to_river failed, closing waybar_fd=%d", waybar_fd);
        close(waybar_fd);
        return;
    }
    plog("setup_client: river_fd=%d", river_fd);

    struct RelayArgs *a1 = malloc(sizeof(*a1));
    struct RelayArgs *a2 = malloc(sizeof(*a2));
    if (!a1 || !a2) {
        free(a1); free(a2);
        close(waybar_fd); close(river_fd);
        return;
    }

    pthread_mutex_lock(&clients_mu);
    struct Client *c = alloc_client();
    if (!c) {
        pthread_mutex_unlock(&clients_mu);
        plog_err("setup_client: no client slot free, closing both fds");
        free(a1); free(a2);
        close(waybar_fd); close(river_fd);
        return;
    }
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->write_mu, NULL);
    c->waybar_fd  = waybar_fd;
    c->river_fd   = river_fd;
    c->next_sid   = SERVER_ID_BASE;
    c->active_threads = 2;
    c->active     = true;
    pthread_mutex_unlock(&clients_mu);

    a1->c = c; a1->src_fd = waybar_fd; a1->dst_fd = river_fd;
    a2->c = c; a2->src_fd = river_fd;  a2->dst_fd = waybar_fd;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t1, t2;
    int r1 = pthread_create(&t1, &attr, relay_c2s, a1);
    int r2 = pthread_create(&t2, &attr, relay_s2c, a2);
    pthread_attr_destroy(&attr);
    plog("setup_client: relay threads started c2s=%d s2c=%d", r1, r2);
}
