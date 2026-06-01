/*
 * maindeck-proxy — Wayland proxy that adds missing output_enter events for River.
 *
 * River's zwlr_foreign_toplevel_manager_v1 never emits output_enter.
 * This proxy sits between any Wayland client (e.g. waybar) and River:
 *
 *   client  ←→  maindeck-proxy  ←→  River
 *
 * All protocols are relayed verbatim — including file descriptor passing via
 * sendmsg/recvmsg with SCM_RIGHTS — EXCEPT zwlr_foreign_toplevel_manager_v1,
 * which the proxy handles itself, adding synthetic output_enter events.
 *
 * Usage in River init:
 *   maindeck-proxy 2>>session.log &
 *   # poll until socket appears
 *   for i in $(seq 20); do
 *       [ -S "$XDG_RUNTIME_DIR/maindeck-0" ] && break; sleep 0.1; done
 *   WAYLAND_DISPLAY=maindeck-0 waybar &
 */

#define _POSIX_C_SOURCE 200809L
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
#include <signal.h>
#include <sys/wait.h>

#include <wayland-client.h>
#include <time.h>
#include <stdarg.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

/* ── Logging ───────────────────────────────────────────────────── */
static void plog(const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *t = localtime(&ts.tv_sec);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", t);
    fprintf(stderr, "[%s.%03ld] [proxy] ", tbuf, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ── Wayland binary protocol helpers ───────────────────────────── */

#define MSG_HDR 8u   /* object_id(4) + size_opcode(4) */
#define BUF_SZ  65536u

static uint32_t ru32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void wu32(uint8_t *p, uint32_t v) {
    p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}

static bool message_has_fds(struct msghdr *mh) {
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(mh);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(mh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            return true;
        }
    }
    return false;
}

/* Relay one chunk of data + ancillary fds from src to dst. */
static ssize_t relay_chunk(int src, int dst) {
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
    if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) return -1;
    return n;
}

/* Write a synthetic Wayland event directly to a client fd (no fds needed). */
static ssize_t raw_write(int fd, pthread_mutex_t *mu,
                         const uint8_t *buf, size_t len) {
    struct iovec iov = { .iov_base = (void*)buf, .iov_len = len };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t r;
    pthread_mutex_lock(mu);
    r = sendmsg(fd, &mh, MSG_NOSIGNAL);
    pthread_mutex_unlock(mu);
    if (r < 0) {
        plog("raw_write: sendmsg fd=%d len=%zu errno=%d (%s)",
             fd, len, errno, strerror(errno));
    } else if ((size_t)r != len) {
        plog("raw_write: short write fd=%d wrote=%zd want=%zu",
             fd, r, len);
    }
    return r;
}

/* Encode a Wayland string (length-prefixed, NUL-terminated, 4-byte padded). */
static int encode_str(uint8_t *out, size_t cap, const char *s) {
    if (!s) s = "";
    uint32_t slen = (uint32_t)strlen(s) + 1;
    uint32_t pad  = (slen + 3u) & ~3u;
    if (4u + pad > cap) return -1;
    wu32(out, slen);
    memcpy(out+4, s, slen);
    memset(out+4+slen, 0, pad-slen);
    return (int)(4u + pad);
}

/* ── Toplevel state ─────────────────────────────────────────────── */

struct Toplevel {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char   *title;
    char   *app_id;
    bool    activated;
    bool    closed;
    struct wl_list link;
};

static struct wl_list  toplevels;
static pthread_mutex_t state_mu = PTHREAD_MUTEX_INITIALIZER;

/* ── Client registry ─────────────────────────────────────────────── */

#define MAX_CLIENTS 8
#define HANDLE_MAP  256

/* WARNING: A fixed high SERVER_ID_BASE is fundamentally broken. libwayland
 * (src/wayland-util.c, wl_map_insert_at) stores server-allocated objects
 * (id >= WL_SERVER_ID_START = 0xff000000) in a DENSE array indexed by
 * (id - 0xff000000), and rejects any insert where that index exceeds the
 * current entry count: `if (count < i) { errno = EINVAL; return -1; }`.
 * So synthetic server IDs MUST continue River's own sequence (the next id is
 * 0xff000000 + current_count) — you cannot pick an arbitrary high base, or
 * the first synthetic handle waybar receives triggers EINVAL and the whole
 * connection dies. This is the waybar crash. The real fix (Direction B) is to
 * NOT synthesize handles at all: forward River's real (dense) handles and
 * inject only output_enter. SERVER_ID_BASE is retained only for the legacy
 * synthesis path, which MAINDECK_PASSTHROUGH=1 disables. */
#define SERVER_ID_BASE 0xFFF00000u
#define FAKE_OUTPUT_GLOBAL 0xE0000000u

/* Runtime toggles (set from env in main).
 * Direction B (forward River's real handles + inject output_enter) is the
 * DEFAULT. The legacy synthetic-handle path (which causes the waybar EINVAL)
 * is kept only as an opt-in escape hatch for A/B comparison in the harness. */
static bool opt_synthesize = false;    /* MAINDECK_SYNTHESIZE=1: re-enable legacy synthetic toplevels */

struct HEntry {
    uint32_t cid;
    uint32_t manager_id;
    struct zwlr_foreign_toplevel_handle_v1 *h;
    bool output_entered;
};

struct Client {
    int            waybar_fd;
    int            river_fd;
    pthread_mutex_t write_mu;

    uint32_t       manager_ids[8]; /* waybar's object IDs for our managers */
    int            manager_n;
    uint32_t       output_id;    /* waybar's first wl_output object ID (0=not seen) */
    uint32_t       output_version;
    bool           output_is_fake;
    uint32_t       compositor_global;
    uint32_t       compositor_version;
    uint32_t       compositor_id;
    uint32_t       fixes_global;
    uint32_t       fixes_version;
    uint32_t       zxdg_output_manager_id;
    uint32_t       zxdg_output_manager_version;
    uint32_t       zwlr_layer_shell_id;
    bool           fake_output_global_sent;
    uint32_t       fake_output_registry_id;

    struct HEntry  hmap[HANDLE_MAP];
    int            hmap_n;
    uint32_t       drop_ids[HANDLE_MAP];
    int            drop_n;
    /* IDs assigned by waybar for layer surfaces (via get_layer_surface new_id).
     * These client-side IDs are echoed back by River in configure events and must
     * NEVER be dropped even if they collide with a toplevel handle drop_id. */
    uint32_t       layer_surface_ids[64];
    int            layer_surface_n;
    uint32_t       registry_ids[32];
    int            registry_n;
    char           unhandled_binds[32][64]; /* iface names already logged */
    int            unhandled_n;
    uint32_t       next_sid;     /* next server-side ID for handle events */

    /* Direction B: River's REAL foreign-toplevel handle IDs as seen on THIS
     * waybar connection (parsed from the s2c stream: manager.toplevel new_id).
     * We forward River's handles untouched and inject output_enter on these
     * real IDs — no synthetic server-id allocation, so no libwayland EINVAL. */
    uint32_t       real_handle_ids[HANDLE_MAP];
    bool           real_handle_entered[HANDLE_MAP]; /* output_enter already injected */
    int            real_handle_n;

    bool           output_done_received;
    bool           active;
};

static struct Client    clients[MAX_CLIENTS];
static pthread_mutex_t  clients_mu = PTHREAD_MUTEX_INITIALIZER;

static void cleanup_client(struct Client *c);

static struct Client *alloc_client(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].active) return &clients[i];
    return NULL;
}

/* ── Binary event builders ──────────────────────────────────────── */

/* zwlr_foreign_toplevel_manager_v1.toplevel(new_id) */
static uint32_t emit_toplevel_new(struct Client *c,
    struct zwlr_foreign_toplevel_handle_v1 *h, uint32_t manager_id) {
    uint32_t sid = c->next_sid++;
    if (c->hmap_n < HANDLE_MAP) {
        c->hmap[c->hmap_n].cid = sid;
        c->hmap[c->hmap_n].manager_id = manager_id;
        c->hmap[c->hmap_n].h   = h;
        c->hmap[c->hmap_n].output_entered = false;
        c->hmap_n++;
    }
    uint8_t msg[12];
    wu32(msg+0, manager_id);
    wu32(msg+4, (12u<<16)|0u); /* opcode 0 = toplevel */
    wu32(msg+8, sid);
    plog("emit_toplevel_new: SENDING synthetic toplevel sid=0x%x (mgr=%u) to waybar "
         "[libwayland needs server ids DENSE from 0xff000000 — this WILL EINVAL]", sid, manager_id);
    raw_write(c->waybar_fd, &c->write_mu, msg, 12);
    return sid;
}

static void emit_str(struct Client *c, uint32_t obj,
                     uint16_t op, const char *s) {
    uint8_t buf[512];
    int sl = encode_str(buf+8, sizeof(buf)-8, s);
    if (sl < 0) return;
    uint32_t tot = 8u + (uint32_t)sl;
    wu32(buf+0, obj);
    wu32(buf+4, (tot<<16)|op);
    raw_write(c->waybar_fd, &c->write_mu, buf, tot);
}

static void emit_state(struct Client *c, uint32_t obj, bool activated) {
    /* wl_array: 4-byte length + payload */
    uint32_t alen = activated ? 4u : 0u;
    uint32_t tot  = 8u + 4u + alen;
    uint8_t  buf[24];
    wu32(buf+0, obj);
    wu32(buf+4, (tot<<16)|4u); /* opcode 4 = state */
    wu32(buf+8, alen);
    if (activated) wu32(buf+12, 2u); /* ACTIVATED = 2 */
    raw_write(c->waybar_fd, &c->write_mu, buf, activated ? 16u : 12u);
}

static void emit_output_enter(struct Client *c, uint32_t obj) {
    if (!c->output_id) return;
    uint8_t msg[12];
    wu32(msg+0, obj);
    wu32(msg+4, (12u<<16)|2u); /* opcode 2 = output_enter */
    wu32(msg+8, c->output_id);
    raw_write(c->waybar_fd, &c->write_mu, msg, 12);
}

/* ── Direction B: real-handle tracking + output_enter injection ──────
 * River forwards real foreign-toplevel handles to waybar untouched; River
 * 0.4.5 just never emits output_enter, so waybar's wlr/taskbar shows no
 * button. We track each real handle ID (from manager.toplevel new_id in the
 * s2c stream) and inject exactly one output_enter on it, using the real
 * wl_output waybar already bound (c->output_id). No server-id allocation. */

static int real_handle_index(struct Client *c, uint32_t hid) {
    for (int i = 0; i < c->real_handle_n; i++)
        if (c->real_handle_ids[i] == hid) return i;
    return -1;
}

/* Inject output_enter for handle hid if the output is known and we haven't
 * already. If the output isn't bound yet, leave it pending for replay on
 * wl_output.done. Caller must hold clients_mu. */
static void inject_output_enter_for(struct Client *c, uint32_t hid) {
    int i = real_handle_index(c, hid);
    if (i < 0) return;
    if (c->real_handle_entered[i]) return;
    if (!c->output_id) return;             /* defer until output bound */
    emit_output_enter(c, hid);
    c->real_handle_entered[i] = true;
    plog("dirB: injected output_enter(handle=%u, output=%u)", hid, c->output_id);
}

/* Record a real handle id seen on the s2c stream. Caller must hold clients_mu. */
static void real_handle_add(struct Client *c, uint32_t hid) {
    if (real_handle_index(c, hid) >= 0) return;
    if (c->real_handle_n >= HANDLE_MAP) return;
    c->real_handle_ids[c->real_handle_n] = hid;
    c->real_handle_entered[c->real_handle_n] = false;
    c->real_handle_n++;
}

/* River sent .closed for a handle: forget it so we never replay onto a dead
 * object. Caller must hold clients_mu. */
static void real_handle_remove(struct Client *c, uint32_t hid) {
    int i = real_handle_index(c, hid);
    if (i < 0) return;
    for (int j = i; j < c->real_handle_n - 1; j++) {
        c->real_handle_ids[j] = c->real_handle_ids[j+1];
        c->real_handle_entered[j] = c->real_handle_entered[j+1];
    }
    c->real_handle_n--;
}

/* Replay output_enter for every known real handle not yet entered (used once
 * the real wl_output's .done arrives). Caller must hold clients_mu. */
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

static void emit_done(struct Client *c, uint32_t obj) {
    uint8_t msg[8];
    wu32(msg+0, obj);
    wu32(msg+4, (8u<<16)|5u); /* opcode 5 = done */
    raw_write(c->waybar_fd, &c->write_mu, msg, 8);
}

static void emit_closed(struct Client *c, uint32_t obj) {
    uint8_t msg[8];
    wu32(msg+0, obj);
    wu32(msg+4, (8u<<16)|6u); /* opcode 6 = closed */
    raw_write(c->waybar_fd, &c->write_mu, msg, 8);
}

static void emit_registry_global(struct Client *c, uint32_t registry_id,
        uint32_t name, const char *iface, uint32_t version) {
    uint8_t buf[256];
    uint32_t off = 8;
    wu32(buf + off, name); off += 4;
    int sl = encode_str(buf + off, sizeof(buf) - off - 4, iface);
    if (sl < 0) return;
    off += (uint32_t)sl;
    wu32(buf + off, version); off += 4;
    wu32(buf+0, registry_id);
    wu32(buf+4, (off<<16)|0u); /* wl_registry.global */
    raw_write(c->waybar_fd, &c->write_mu, buf, off);
}

static void emit_fake_output_events(struct Client *c) {
    if (!c->output_id || !c->output_is_fake) return;

    uint8_t buf[512];
    uint32_t off = 8;
    wu32(buf + off, 0); off += 4;       /* x */
    wu32(buf + off, 0); off += 4;       /* y */
    wu32(buf + off, 0); off += 4;       /* physical_width */
    wu32(buf + off, 0); off += 4;       /* physical_height */
    wu32(buf + off, 0); off += 4;       /* subpixel unknown */
    int sl = encode_str(buf + off, sizeof(buf) - off, "maindeck");
    if (sl < 0) return;
    off += (uint32_t)sl;
    sl = encode_str(buf + off, sizeof(buf) - off, "synthetic");
    if (sl < 0) return;
    off += (uint32_t)sl;
    wu32(buf + off, 0); off += 4;       /* transform normal */
    wu32(buf+0, c->output_id);
    wu32(buf+4, (off<<16)|0u);          /* wl_output.geometry */
    raw_write(c->waybar_fd, &c->write_mu, buf, off);

    uint8_t mode[24];
    wu32(mode+0, c->output_id);
    wu32(mode+4, (24u<<16)|1u);         /* wl_output.mode */
    wu32(mode+8, 3u);                   /* current + preferred */
    wu32(mode+12, 1920u);
    wu32(mode+16, 1080u);
    wu32(mode+20, 60000u);
    raw_write(c->waybar_fd, &c->write_mu, mode, sizeof(mode));

    if (c->output_version >= 2) {
        uint8_t scale[12];
        wu32(scale+0, c->output_id);
        wu32(scale+4, (12u<<16)|3u);    /* wl_output.scale */
        wu32(scale+8, 1u);
        raw_write(c->waybar_fd, &c->write_mu, scale, sizeof(scale));
    }

    if (c->output_version >= 4) {
        emit_str(c, c->output_id, 4, "maindeck");          /* wl_output.name */
        emit_str(c, c->output_id, 5, "MainDeck synthetic output");
    }

    if (c->output_version >= 2) {
        uint8_t done[8];
        wu32(done+0, c->output_id);
        wu32(done+4, (8u<<16)|2u);      /* wl_output.done */
        raw_write(c->waybar_fd, &c->write_mu, done, sizeof(done));
    }
}

static void emit_fake_xdg_output_events(struct Client *c, uint32_t xdg_output_id) {
    uint8_t pos[16];
    wu32(pos+0, xdg_output_id);
    wu32(pos+4, (16u<<16)|0u);          /* zxdg_output_v1.logical_position */
    wu32(pos+8, 0);
    wu32(pos+12, 0);
    raw_write(c->waybar_fd, &c->write_mu, pos, sizeof(pos));

    uint8_t size[16];
    wu32(size+0, xdg_output_id);
    wu32(size+4, (16u<<16)|1u);         /* zxdg_output_v1.logical_size */
    wu32(size+8, 1920u);
    wu32(size+12, 1080u);
    raw_write(c->waybar_fd, &c->write_mu, size, sizeof(size));

    if (c->zxdg_output_manager_version >= 2) {
        emit_str(c, xdg_output_id, 3, "maindeck"); /* zxdg_output_v1.name */
        emit_str(c, xdg_output_id, 4, "MainDeck synthetic output");
    }

    uint8_t done[8];
    wu32(done+0, xdg_output_id);
    wu32(done+4, (8u<<16)|2u);          /* zxdg_output_v1.done */
    raw_write(c->waybar_fd, &c->write_mu, done, sizeof(done));
}

static bool client_is_manager_id(struct Client *c, uint32_t id) {
    for (int i = 0; i < c->manager_n; i++)
        if (c->manager_ids[i] == id) return true;
    return false;
}

static bool client_is_layer_surface_id(struct Client *c, uint32_t id) {
    for (int i = 0; i < c->layer_surface_n; i++)
        if (c->layer_surface_ids[i] == id) return true;
    return false;
}

static void client_add_layer_surface_id(struct Client *c, uint32_t id) {
    if (client_is_layer_surface_id(c, id)) return;
    if (c->layer_surface_n < 64) c->layer_surface_ids[c->layer_surface_n++] = id;
}

static uint32_t client_sid(struct Client *c,
    struct zwlr_foreign_toplevel_handle_v1 *h, uint32_t manager_id) {
    for (int i = 0; i < c->hmap_n; i++)
        if (c->hmap[i].h == h && c->hmap[i].manager_id == manager_id) return c->hmap[i].cid;
    return 0;
}

static struct HEntry *client_hentry_for_sid(struct Client *c, uint32_t sid) {
    for (int i = 0; i < c->hmap_n; i++)
        if (c->hmap[i].cid == sid) return &c->hmap[i];
    return NULL;
}

static bool client_should_drop_server_id(struct Client *c, uint32_t id) {
    /* Never drop a layer_surface ID: the configure event must reach waybar. */
    if (client_is_layer_surface_id(c, id)) return false;
    for (int i = 0; i < c->drop_n; i++)
        if (c->drop_ids[i] == id) return true;
    return false;
}

static bool client_is_synthetic_handle_id(struct Client *c, uint32_t id) {
    return id >= SERVER_ID_BASE && id < c->next_sid;
}

static void client_add_drop_server_id(struct Client *c, uint32_t id) {
    if (client_is_layer_surface_id(c, id)) return;  /* never drop layer_surface IDs */
    if (client_should_drop_server_id(c, id)) return;
    if (c->drop_n < HANDLE_MAP) c->drop_ids[c->drop_n++] = id;
}

static bool client_is_registry_id(struct Client *c, uint32_t id) {
    for (int i = 0; i < c->registry_n; i++)
        if (c->registry_ids[i] == id) return true;
    return false;
}

static void client_add_registry_id(struct Client *c, uint32_t id) {
    if (client_is_registry_id(c, id)) return;
    if (c->registry_n < 32) c->registry_ids[c->registry_n++] = id;
}

static void emit_output_enter_once(struct Client *c, struct HEntry *entry) {
    if (!c->output_id || !c->output_done_received || entry->output_entered) return;
    emit_output_enter(c, entry->cid);
    entry->output_entered = true;
}

static void replay_output_enters(struct Client *c) {
    if (!c->output_id) return;
    for (int i = 0; i < c->hmap_n; i++) {
        if (c->hmap[i].output_entered) continue;
        emit_output_enter_once(c, &c->hmap[i]);
        emit_done(c, c->hmap[i].cid);
    }
}

static void flush_toplevel(struct Client *c, struct Toplevel *t) {
    /* Direction B (default): never synthesize. River's own real handle is
     * forwarded to waybar by relay_s2c; output_enter is injected there. */
    if (!opt_synthesize) return;
    for (int i = 0; i < c->manager_n; i++) {
        uint32_t manager_id = c->manager_ids[i];
        if (client_sid(c, t->handle, manager_id)) continue;
        uint32_t sid = emit_toplevel_new(c, t->handle, manager_id);
        struct HEntry *entry = client_hentry_for_sid(c, sid);
        emit_str         (c, sid, 0, t->title   ? t->title   : "");
        emit_str         (c, sid, 1, t->app_id  ? t->app_id  : "");
        emit_state       (c, sid, t->activated);
        if (entry) emit_output_enter_once(c, entry);
        emit_done        (c, sid);
    }
}

/* Called from wayland_thread when a toplevel changes */
static void broadcast_update(struct Toplevel *t) {
    /* Direction B (default): River's own handle stream (forwarded by relay_s2c)
     * is authoritative; nothing to synthesize or update here. */
    if (!opt_synthesize) return;
    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (!c->active || !c->manager_n) continue;
        for (int m = 0; m < c->manager_n; m++) {
            uint32_t manager_id = c->manager_ids[m];
            uint32_t sid = client_sid(c, t->handle, manager_id);
            if (!sid) {
                sid = emit_toplevel_new(c, t->handle, manager_id);
                struct HEntry *entry = client_hentry_for_sid(c, sid);
                emit_str         (c, sid, 0, t->title   ? t->title   : "");
                emit_str         (c, sid, 1, t->app_id  ? t->app_id  : "");
                emit_state       (c, sid, t->activated);
                if (entry) emit_output_enter_once(c, entry);
                emit_done        (c, sid);
            } else {
                emit_str         (c, sid, 0, t->title   ? t->title   : "");
                emit_str         (c, sid, 1, t->app_id  ? t->app_id  : "");
                emit_state       (c, sid, t->activated);
                struct HEntry *entry = client_hentry_for_sid(c, sid);
                if (entry) emit_output_enter_once(c, entry);
                emit_done        (c, sid);
            }
        }
    }
    pthread_mutex_unlock(&clients_mu);
}

static void broadcast_closed(struct Toplevel *t) {
    pthread_mutex_lock(&clients_mu);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &clients[i];
        if (!c->active || !c->manager_n) continue;
        for (int m = 0; m < c->manager_n; m++) {
            uint32_t manager_id = c->manager_ids[m];
            uint32_t sid = client_sid(c, t->handle, manager_id);
            if (sid) emit_closed(c, sid);
        }
    }
    pthread_mutex_unlock(&clients_mu);
}

/* ── Toplevel listeners (proxy's own River connection) ─────────── */

static void tl_title(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h; struct Toplevel *t = data;
    pthread_mutex_lock(&state_mu);
    free(t->title); t->title = s ? strdup(s) : NULL;
    pthread_mutex_unlock(&state_mu);
}
static void tl_app_id(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h; struct Toplevel *t = data;
    pthread_mutex_lock(&state_mu);
    free(t->app_id); t->app_id = s ? strdup(s) : NULL;
    pthread_mutex_unlock(&state_mu);
}
static void tl_oe(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d;(void)h;(void)o; }
static void tl_ol(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct wl_output *o) { (void)d;(void)h;(void)o; }
static void tl_state(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st) {
    (void)h; struct Toplevel *t = data;
    bool act = false;
    uint32_t *s;
    wl_array_for_each(s, st)
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) act = true;
    pthread_mutex_lock(&state_mu);
    t->activated = act;
    pthread_mutex_unlock(&state_mu);
}
static void tl_done(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h; struct Toplevel *t = data;
    pthread_mutex_lock(&state_mu);
    bool closed = t->closed;
    pthread_mutex_unlock(&state_mu);
    if (!closed) broadcast_update(t);
}
static void tl_closed(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h; struct Toplevel *t = data;
    broadcast_closed(t);
    pthread_mutex_lock(&state_mu);
    t->closed = true;
    wl_list_remove(&t->link);
    zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
    free(t->title); free(t->app_id);
    pthread_mutex_unlock(&state_mu);
    free(t);
}
static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
    struct zwlr_foreign_toplevel_handle_v1 *p) { (void)d;(void)h;(void)p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
    .title=tl_title, .app_id=tl_app_id,
    .output_enter=tl_oe, .output_leave=tl_ol,
    .state=tl_state, .done=tl_done,
    .closed=tl_closed, .parent=tl_parent,
};

static void mgr_toplevel(void *data,
    struct zwlr_foreign_toplevel_manager_v1 *mgr,
    struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)data;(void)mgr;
    struct Toplevel *t = calloc(1, sizeof(*t));
    if (!t) return;
    t->handle = h;
    pthread_mutex_lock(&state_mu);
    wl_list_insert(toplevels.prev, &t->link);
    pthread_mutex_unlock(&state_mu);
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, t);
}
static void mgr_finished(void *d,
    struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d;(void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_listener = {
    .toplevel=mgr_toplevel, .finished=mgr_finished,
};

static struct zwlr_foreign_toplevel_manager_v1 *proxy_manager = NULL;

static void reg_global(void *d, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
        proxy_manager = wl_registry_bind(reg, name,
            &zwlr_foreign_toplevel_manager_v1_interface, ver<3?ver:3);
        zwlr_foreign_toplevel_manager_v1_add_listener(
            proxy_manager, &mgr_listener, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener =
    { .global=reg_global, .global_remove=reg_remove };

/* ── wl_registry.bind interception ─────────────────────────────── */

/*
 * Parse wl_registry.bind binary message.
 * Format after 8-byte header:
 *   uint32 name
 *   uint32 iface_len  (including NUL)
 *   bytes  iface_str  (padded to 4 bytes)
 *   uint32 version
 *   uint32 new_id
 */
static bool parse_bind(const uint8_t *msg, size_t msz,
    uint32_t *name, char *iface, size_t iface_cap,
    uint32_t *ver, uint32_t *new_id) {
    if (msz < MSG_HDR + 4u) return false;
    size_t off = MSG_HDR;
    *name = ru32(msg+off); off += 4;
    if (off+4 > msz) return false;
    uint32_t slen = ru32(msg+off); off += 4;
    uint32_t pad  = (slen+3u)&~3u;
    if (off+pad+8 > msz) return false;
    size_t cp = slen < iface_cap ? slen : iface_cap-1;
    memcpy(iface, msg+off, cp); iface[cp] = '\0';
    off += pad;
    *ver    = ru32(msg+off); off += 4;
    *new_id = ru32(msg+off);
    return true;
}

static bool parse_global(const uint8_t *msg, size_t msz,
    uint32_t *name, char *iface, size_t iface_cap, uint32_t *ver) {
    if (msz < MSG_HDR + 4u) return false;
    size_t off = MSG_HDR;
    *name = ru32(msg+off); off += 4;
    if (off+4 > msz) return false;
    uint32_t slen = ru32(msg+off); off += 4;
    uint32_t pad  = (slen+3u)&~3u;
    if (off+pad+4 > msz) return false;
    size_t cp = slen < iface_cap ? slen : iface_cap-1;
    memcpy(iface, msg+off, cp); iface[cp] = '\0';
    off += pad;
    *ver = ru32(msg+off);
    return true;
}

static size_t build_bind(uint8_t *out, size_t cap, uint32_t registry_id,
    uint32_t name, const char *iface, uint32_t version, uint32_t new_id) {
    if (cap < MSG_HDR + 16u) return 0;
    uint32_t off = 8;
    wu32(out + off, name); off += 4;
    int sl = encode_str(out + off, cap - off - 8, iface);
    if (sl < 0) return 0;
    off += (uint32_t)sl;
    wu32(out + off, version); off += 4;
    wu32(out + off, new_id); off += 4;
    wu32(out+0, registry_id);
    wu32(out+4, (off<<16)|0u);
    return off;
}

static bool rewrite_fake_output_bind_inplace(struct Client *c, uint8_t *msg, size_t msz) {
    uint32_t name, ver, new_id;
    char iface[128];
    if (!parse_bind(msg, msz, &name, iface, sizeof(iface), &ver, &new_id)) return false;
    if (name != FAKE_OUTPUT_GLOBAL || strcmp(iface, "wl_output") != 0) return false;
    if (!c->fixes_global) return false;

    c->output_id = new_id;
    c->output_version = ver;
    c->output_is_fake = true;

    size_t off = MSG_HDR;
    wu32(msg + off, c->fixes_global); off += 4;
    uint32_t slen = ru32(msg + off); off += 4;
    uint32_t pad = (slen + 3u) & ~3u;
    memset(msg + off, 0, pad);
    memcpy(msg + off, "wl_fixes", sizeof("wl_fixes"));
    off += pad;
    uint32_t fver = c->fixes_version < ver ? c->fixes_version : ver;
    if (fver == 0) fver = 1;
    wu32(msg + off, fver);

    emit_fake_output_events(c);
    return true;
}

/* ── Per-client relay threads ──────────────────────────────────── */

struct RelayArgs { struct Client *c; int src_fd; int dst_fd; };

/*
 * waybar → River relay.
 * Intercepts wl_registry.bind for zwlr_foreign_toplevel_manager_v1.
 * Everything else (including fd passing) relayed verbatim.
 *
 * We need to parse messages selectively, but fd passing must still work.
 * Strategy: read one message at a time using recvmsg; if we don't need to
 * intercept, forward the raw bytes + ancillary data to River.
 */
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

    /* We accumulate data until we have a full message, then decide
     * whether to intercept or forward.  FDs in ancillary data are
     * forwarded whenever present in a relay chunk. */
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

        if (sizeof(buf) - pending == 0) {
            /* Buffer full — can't parse this huge message; just forward raw */
            relay_chunk(src, dst);
            continue;
        }

        ssize_t n = recvmsg(src, &mh, 0);
        if (n <= 0) break;

        /* If this chunk carries ancillary data (fds), we MUST forward it
         * verbatim — sending a 0-byte fd message would make River treat
         * it as EOF on the stream socket.  fd-bearing chunks never carry
         * wl_registry.bind, so skipping parse is safe. */
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
	                                    snprintf(c->unhandled_binds[c->unhandled_n], 64, "%s", iface);
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
	                        /* get_layer_surface in ancillary path - track the new_id */
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
	                    if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) goto done;
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
            if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) goto done;
            /* don't add to pending — forwarded verbatim */
            continue;
        }

        pending += (size_t)n;

        /* Process complete messages in the buffer */
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

	            /* Intercept wl_registry.bind */
	            if (client_is_registry_id(c, obj_id) && opcode == 0) {
                uint32_t gname, gver, new_id;
                char iface[128];
	                if (parse_bind(msg, msize, &gname, iface,
	                               sizeof(iface), &gver, &new_id)) {
	                    if (!strcmp(iface, "zwlr_foreign_toplevel_manager_v1")) {
                        /* Forward the bind to River so the client's object ID
                         * namespace stays contiguous, then send our corrected
                         * toplevel stream after River has seen the manager. */
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

                    } else if (!strcmp(iface, "wl_output") &&
                               !c->output_id) {
                        c->output_id = new_id; /* remember first wl_output */
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
                        wu32(create_region+4, (12u<<16)|1u); /* wl_compositor.create_region */
                        wu32(create_region+8, xdg_output_id);
                        if (send(dst, create_region, sizeof(create_region), MSG_NOSIGNAL) < 0) goto done;
                    }
                    emit_fake_xdg_output_events(c, xdg_output_id);
                    drop = true;
                }
            }

            if (!drop && c->zwlr_layer_shell_id &&
                    obj_id == c->zwlr_layer_shell_id && opcode == 0 && msize >= 24) {
                /* get_layer_surface(new_id, surface, output, layer, namespace)
                 * Track the new_id so relay_s2c never drops the configure event. */
                uint32_t ls_new_id = ru32(msg + 8);
                client_add_layer_surface_id(c, ls_new_id);
                plog("relay_c2s: get_layer_surface new_id=%u tracked", ls_new_id);
                if (c->output_is_fake) {
                    /* get_layer_surface é pequeno (5 args + namespace curto);
                     * 256B bastam. O guard mantém o caso patológico seguro:
                     * se algo vier maior, não reescreve (cai no forward normal). */
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

        /* Compact buffer */
        if (off > 0) {
            pending -= off;
            if (pending) memmove(buf, buf+off, pending);
        }
    }
done:
    plog("relay_c2s: ending src=%d dst=%d errno=%d", src, dst, errno);
    cleanup_client(c);
    return NULL;
}

/* River → client relay. Filter River's original foreign-toplevel stream because
 * the proxy sends a corrected copy with output_enter events. */
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
                    plog("relay_s2c: send(pending) failed errno=%d", errno);
                    break;
                }
            }
            iov.iov_base = buf + pending;
            iov.iov_len  = (size_t)n;
            mh.msg_flags = 0;
            if (sendmsg(dst, &mh, MSG_NOSIGNAL) < 0) {
                pthread_mutex_unlock(&c->write_mu);
                plog("relay_s2c: sendmsg(anc) failed errno=%d", errno);
                break;
            }
            pending = 0;
            pthread_mutex_unlock(&c->write_mu);
            continue;
        }

        /* Anomaly-only diagnostics: log wl_display.error (always fatal),
         * malformed messages, and potential server-ID collisions where
         * River allocates an ID that falls within the proxy's synthetic
         * range — that class of bug causes waybar EINVAL / configure timeout. */
        if (pending == 0 && n >= 8) {
            size_t scan = 0;
            while (scan + 8 <= (size_t)n) {
                uint32_t obj = ru32(buf+scan);
                uint32_t so = ru32(buf+scan+4);
                uint32_t sz = so >> 16;
                uint32_t op = so & 0xffff;
                if (sz < 8 || scan + sz > (size_t)n) {
                    plog("relay_s2c: !!! malformed msg at off=%zu obj=%u op=%u size=%u chunk=%zd",
                         scan, obj, op, sz, n);
                    break;
                }
                if (obj == 1 && op == 0 && sz >= 16) {
                    uint32_t target = ru32(buf+scan+8);
                    uint32_t code = ru32(buf+scan+12);
                    uint32_t slen = ru32(buf+scan+16);
                    const char *s = (const char *)(buf+scan+20);
                    plog("relay_s2c: *** wl_display.error target=%u code=%u msg='%.*s'",
                         target, code, (int)slen, s);
                } else if (obj >= SERVER_ID_BASE && obj < c->next_sid &&
                           !client_should_drop_server_id(c, obj)) {
                    plog("relay_s2c: !!! server-ID collision: River obj=0x%x op=%u falls in proxy synthetic range [0x%x..0x%x)",
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
            bool emit_fake_global_after = false;

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
            /* Newly-created real handle id, if this is manager.toplevel(new_id).
             * In Direction B we inject output_enter on it after forwarding. */
            uint32_t new_real_handle = 0;
            bool     handle_closed   = false;
            if (c->manager_n > 0 && client_is_manager_id(c, obj_id) && opcode == 0 && msize >= 12) {
                uint32_t hid = ru32(msg+8);
                if (opt_synthesize) {
                    /* Legacy: suppress River's real handle; a synthetic one
                     * (allocated by the proxy) replaces it. This is the path
                     * that causes the waybar EINVAL. */
                    client_add_drop_server_id(c, hid);
                    drop = true;
                } else {
                    /* Direction B: keep River's real handle; remember it so we
                     * can inject output_enter once it has been forwarded. */
                    real_handle_add(c, hid);
                    new_real_handle = hid;
                }
            } else if (client_should_drop_server_id(c, obj_id)) {
                /* drop_ids are only populated in synthesis mode */
                drop = true;
            } else if (!opt_synthesize && opcode == 6 && real_handle_index(c, obj_id) >= 0) {
                /* zwlr_foreign_toplevel_handle_v1.closed — forward it, then
                 * forget the handle so we don't replay output_enter onto it. */
                handle_closed = true;
            }
            pthread_mutex_unlock(&clients_mu);

            if (!drop) {
                pthread_mutex_lock(&c->write_mu);
                if (send(dst, msg, msize, MSG_NOSIGNAL) < 0) {
                    pthread_mutex_unlock(&c->write_mu);
                    plog("relay_s2c: send(dst=%d) failed obj=%u op=%u size=%u errno=%d (%s)",
                         dst, obj_id, opcode, msize, errno, strerror(errno));
                    goto done_s2c;
                }
                pthread_mutex_unlock(&c->write_mu);
            }

            /* Direction B: inject output_enter immediately after River's
             * toplevel(new_id) has been forwarded. It lands before River's
             * .done for the handle, so waybar commits the button correctly. */
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
                replay_output_enters(c);        /* legacy synthetic handles */
                replay_real_output_enters(c);   /* Direction B real handles */
                pthread_mutex_unlock(&clients_mu);
            }

            /* DISABLED fake_output injection — suspected cause of River
             * closing per-client connections at boot. Without this, waybar
             * binds only the REAL wl_outputs that River advertises. The proxy
             * still needs to emit output_enter events for foreign_toplevel
             * handles using one of those real outputs (see emit_output_enter
             * which uses c->output_id — that's now the first real output). */
            (void)emit_fake_global_after;
            off += msize;
        }

        if (off > 0) {
            pending -= off;
            if (pending) memmove(buf, buf+off, pending);
        }
    }
done_s2c:
    plog("relay_s2c: ending src=%d dst=%d errno=%d", src, dst, errno);
    cleanup_client(c);
    return NULL;
}

/* ── Per-client setup ──────────────────────────────────────────── */

static bool connect_to_river(int *fd_out) {
    const char *dir  = getenv("XDG_RUNTIME_DIR");
    const char *disp = getenv("WAYLAND_DISPLAY");
    if (!dir || !disp) {
        plog("connect_to_river: missing env XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s",
             dir ? dir : "(null)", disp ? disp : "(null)");
        return false;
    }

    char path[256];
    if (disp[0] == '/')
        snprintf(path, sizeof(path), "%s", disp);
    else
        snprintf(path, sizeof(path), "%s/%s", dir, disp);

    int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (fd < 0) { plog("connect_to_river: socket() failed errno=%d", errno); return false; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        plog("connect_to_river: connect(%s) failed errno=%d (%s)", path, errno, strerror(errno));
        close(fd); return false;
    }
    *fd_out = fd;
    return true;
}

static void cleanup_client(struct Client *c) {
    pthread_mutex_lock(&clients_mu);
    if (c->active) {
        c->active = false;
        if (c->waybar_fd >= 0) {
            close(c->waybar_fd);
            c->waybar_fd = -1;
        }
        if (c->river_fd >= 0) {
            close(c->river_fd);
            c->river_fd = -1;
        }
        c->manager_n = 0;
        c->hmap_n = 0;
        c->drop_n = 0;
        c->layer_surface_n = 0;
        c->registry_n = 0;
        c->unhandled_n = 0;
        c->output_id = 0;
        c->output_done_received = false;
    }
    pthread_mutex_unlock(&clients_mu);
}

static void setup_client(int waybar_fd) {
    plog("setup_client: waybar_fd=%d, connecting to River", waybar_fd);
    int river_fd;
    if (!connect_to_river(&river_fd)) {
        plog("setup_client: connect_to_river failed, closing waybar_fd=%d", waybar_fd);
        close(waybar_fd);
        return;
    }
    plog("setup_client: river_fd=%d", river_fd);

    pthread_mutex_lock(&clients_mu);
    struct Client *c = alloc_client();
    if (!c) {
        pthread_mutex_unlock(&clients_mu);
        plog("setup_client: no client slot free, closing both fds");
        close(waybar_fd); close(river_fd);
        return;
    }
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->write_mu, NULL);
    c->waybar_fd  = waybar_fd;
    c->river_fd   = river_fd;
    c->next_sid   = SERVER_ID_BASE;
    c->active     = true;
    pthread_mutex_unlock(&clients_mu);

    struct RelayArgs *a1 = malloc(sizeof(*a1));
    struct RelayArgs *a2 = malloc(sizeof(*a2));
    if (!a1 || !a2) { free(a1); free(a2); close(waybar_fd); close(river_fd); return; }

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

/* ── libwayland event loop thread ──────────────────────────────── */

static struct wl_display *proxy_display = NULL;

static void *wayland_thread(void *arg) {
    (void)arg;
    while (wl_display_dispatch(proxy_display) != -1) {}
    return NULL;
}

/* ── River readiness probe ─────────────────────────────────────── */

/*
 * At session start, River accepts new client connections but closes them
 * within ~7ms for ~13 seconds — likely a session/DRM transition during which
 * libseat is still handing off the seat from SDDM. Existing connections
 * (like the proxy's own proxy_display) survive, but new ones don't.
 *
 * If we expose maindeck-0 before River settles, waybar gets ~22 failed
 * connection attempts in a row before one finally sticks, giving the user
 * a 20s black screen.
 *
 * Workaround: open a probe connection, send get_registry, see if it stays
 * alive for 600ms. Retry until it does. THEN create maindeck-0.
 */
static bool river_probe_stable(void) {
    int fd;
    if (!connect_to_river(&fd)) return false;

    /* Send wl_display.get_registry(new_id=2) — same first message waybar
     * would send. If River is going to close us, it does it after we send
     * something, not on bare connection. */
    uint8_t getreg[12];
    wu32(getreg+0, 1);              /* wl_display */
    wu32(getreg+4, (12u<<16)|1u);   /* opcode 1 = get_registry, size 12 */
    wu32(getreg+8, 2);              /* new_id */
    if (send(fd, getreg, sizeof(getreg), MSG_NOSIGNAL) < 0) {
        close(fd);
        return false;
    }

    /* Poll for 600ms. If River sends EOF in that window, it's not stable
     * yet. If it stays open, we consider it stable. */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int remaining_ms = 600;
    while (remaining_ms > 0) {
        int slice = remaining_ms < 100 ? remaining_ms : 100;
        int r = poll(&pfd, 1, slice);
        if (r < 0) { close(fd); return false; }
        if (r > 0) {
            if (pfd.revents & (POLLHUP|POLLERR|POLLNVAL)) {
                close(fd); return false;
            }
            if (pfd.revents & POLLIN) {
                /* Drain available bytes; treat EOF (0) as not-stable */
                uint8_t buf[4096];
                ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n == 0) { close(fd); return false; }
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(fd); return false;
                }
            }
        }
        remaining_ms -= slice;
    }
    close(fd);
    return true;
}

static void wait_for_river_stable(void) {
    plog("probing River stability before exposing maindeck-0");
    int attempt = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!river_probe_stable()) {
        attempt++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
            + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (attempt % 5 == 0) {
            plog("River still unstable after %d attempts (%ldms elapsed)",
                 attempt, elapsed_ms);
        }
        if (elapsed_ms > 60000) {
            plog("River readiness probe gave up after 60s — proceeding anyway");
            return;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 300 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    struct timespec done;
    clock_gettime(CLOCK_MONOTONIC, &done);
    long elapsed_ms = (done.tv_sec - start.tv_sec) * 1000L
        + (done.tv_nsec - start.tv_nsec) / 1000000L;
    plog("River stable after %d attempts (%ldms)", attempt + 1, elapsed_ms);
}

/* ── Server socket ─────────────────────────────────────────────── */

static int create_server_socket(char *out_name, size_t cap) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir) return -1;
	for (int i = 0; i < 32; i++) {
		snprintf(out_name, cap, "maindeck-%d", i);
		char path[256];
		snprintf(path, sizeof(path), "%s/%s", dir, out_name);

		struct sockaddr_un addr = { .sun_family = AF_UNIX };
		strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);

		int probe = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
		if (probe >= 0) {
			if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				close(probe);
				continue;
			}
			close(probe);
			if (errno == ECONNREFUSED || errno == ENOENT) {
				unlink(path);
			}
		}

		int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
		if (fd < 0) return -1;
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			listen(fd, 8);
			return fd;
		}
        close(fd);
    }
    return -1;
}

/* ── Master-Worker helper functions ────────────────────────────── */

static int send_fd(int sock_fd, int fd_to_send) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char dummy = 'x';
    iov[0].iov_base = &dummy;
    iov[0].iov_len = 1;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd_to_send;

    if (sendmsg(sock_fd, &msg, 0) < 0) {
        return -1;
    }
    return 0;
}

static int recv_fd(int sock_fd) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char dummy;
    iov[0].iov_base = &dummy;
    iov[0].iov_len = 1;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    if (recvmsg(sock_fd, &msg, 0) < 0) {
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        return -1;
    }

    return *((int *)CMSG_DATA(cmsg));
}

static volatile sig_atomic_t child_exited = 0;
static void handle_sigchld(int sig) {
    (void)sig;
    child_exited = 1;
}

static pid_t spawn_worker(int *control_fd) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        plog("Master: socketpair failed: %s", strerror(errno));
        return -1;
    }

    fcntl(pair[1], F_SETFD, 0);

    pid_t pid = fork();
    if (pid < 0) {
        plog("Master: fork failed: %s", strerror(errno));
        close(pair[0]);
        close(pair[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process (Worker) */
        close(pair[0]);
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", pair[1]);
        
        char *const argv[] = { "/proc/self/exe", "--worker", fd_str, NULL };
        execv("/proc/self/exe", argv);
        
        /* Fallbacks */
        execv("./build/maindeck-proxy", argv);
        execv("maindeck-proxy", argv);
        
        plog("Child: exec failed: %s", strerror(errno));
        exit(1);
    }

    /* Parent process (Master) */
    close(pair[1]);
    *control_fd = pair[0];
    plog("Master: spawned Worker PID %d", pid);
    return pid;
}

static void run_worker(int control_fd) {
    plog("Worker: starting (control_fd=%d)", control_fd);
    
    wl_list_init(&toplevels);
    memset(clients, 0, sizeof(clients));

    proxy_display = wl_display_connect(NULL);
    if (!proxy_display) {
        plog("Worker: cannot connect to Wayland (errno=%d %s)", errno, strerror(errno));
        exit(1);
    }
    plog("Worker: connected to River display, doing first roundtrip");

    struct wl_registry *reg = wl_display_get_registry(proxy_display);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(proxy_display);
    if (!proxy_manager) {
        plog("Worker: zwlr_foreign_toplevel_manager_v1 not available");
        exit(1);
    }
    plog("Worker: foreign_toplevel_manager bound, doing second roundtrip for initial toplevels");
    wl_display_roundtrip(proxy_display); /* receive initial toplevels */
    plog("Worker: second roundtrip done");

    pthread_t wl_thr;
    pthread_create(&wl_thr, NULL, wayland_thread, NULL);
    plog("Worker: wayland_thread started, entering FD reception loop");

    while (1) {
        int cfd = recv_fd(control_fd);
        if (cfd < 0) {
            plog("Worker: recv_fd returned error (control socket closed), exiting");
            break;
        }
        plog("Worker: received accepted client connection fd=%d", cfd);
        setup_client(cfd);
    }
    
    plog("Worker: exiting");
    exit(0);
}

static void run_master(int srv) {
    plog("Master: starting");

    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    int control_fd = -1;
    pid_t worker_pid = spawn_worker(&control_fd);
    if (worker_pid < 0) {
        plog("Master: failed to spawn initial worker, exiting");
        return;
    }

    while (1) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                if (child_exited) {
                    child_exited = 0;
                    int status;
                    pid_t reaped = waitpid(worker_pid, &status, WNOHANG);
                    if (reaped == worker_pid) {
                        plog("Master: Worker PID %d exited with status %d, restarting...", worker_pid, status);
                        close(control_fd);
                        worker_pid = spawn_worker(&control_fd);
                        if (worker_pid < 0) {
                            plog("Master: failed to restart worker, exiting");
                            break;
                        }
                    }
                }
                continue;
            }
            plog("Master: accept failed: %s", strerror(errno));
            break;
        }

        plog("Master: accepted new client connection cfd=%d, handing off to Worker", cfd);
        if (send_fd(control_fd, cfd) < 0) {
            plog("Master: failed to send fd to Worker, closing cfd");
        }
        close(cfd);
    }
}

/* ── main ──────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Read runtime toggles from env (inherited across the worker re-exec).
     * Default is Direction B (no synthesis). MAINDECK_SYNTHESIZE=1 opts back
     * into the legacy synthetic-handle path for A/B comparison. */
    const char *syn = getenv("MAINDECK_SYNTHESIZE");
    opt_synthesize = (syn && syn[0] && syn[0] != '0');

    if (argc >= 3 && strcmp(argv[1], "--worker") == 0) {
        int control_fd = atoi(argv[2]);
        plog("Worker: mode=%s", opt_synthesize ? "LEGACY-SYNTHESIS" : "DIRECTION-B (forward real handles + inject output_enter)");
        run_worker(control_fd);
        return 0;
    }

    plog("Master starting (WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s synthesize=%d)",
         getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(null)",
         getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(null)",
         opt_synthesize);

    wait_for_river_stable();

    char name[64];
    int srv = create_server_socket(name, sizeof(name));
    if (srv < 0) {
        plog("Master: cannot create server socket");
        return 1;
    }

    /* Print socket name so the init script can use it */
    printf("%s\n", name);
    fflush(stdout);
    plog("Master listening on %s/%s",
         getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "?", name);

    run_master(srv);

    close(srv);
    return 0;
}
