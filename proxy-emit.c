#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <wayland-client-core.h>

#include "proxy-types.h"
#include "proxy-log.h"
#include "proxy-state.h"
#include "proxy-wire.h"
#include "proxy-emit.h"

uint32_t emit_toplevel_new(struct Client *c,
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

void emit_str(struct Client *c, uint32_t obj,
                     uint16_t op, const char *s) {
    uint8_t buf[512];
    int sl = encode_str(buf+8, sizeof(buf)-8, s);
    if (sl < 0) return;
    uint32_t tot = 8u + (uint32_t)sl;
    wu32(buf+0, obj);
    wu32(buf+4, (tot<<16)|op);
    raw_write(c->waybar_fd, &c->write_mu, buf, tot);
}

void emit_state(struct Client *c, uint32_t obj, bool activated) {
    uint32_t alen = activated ? 4u : 0u;
    uint32_t tot  = 8u + 4u + alen;
    uint8_t  buf[24];
    wu32(buf+0, obj);
    wu32(buf+4, (tot<<16)|4u); /* opcode 4 = state */
    wu32(buf+8, alen);
    if (activated) wu32(buf+12, 2u); /* ACTIVATED = 2 */
    raw_write(c->waybar_fd, &c->write_mu, buf, activated ? 16u : 12u);
}

void emit_output_enter(struct Client *c, uint32_t obj) {
    if (!c->output_id) return;
    uint8_t msg[12];
    wu32(msg+0, obj);
    wu32(msg+4, (12u<<16)|2u); /* opcode 2 = output_enter */
    wu32(msg+8, c->output_id);
    raw_write(c->waybar_fd, &c->write_mu, msg, 12);
}

void emit_done(struct Client *c, uint32_t obj) {
    uint8_t msg[8];
    wu32(msg+0, obj);
    wu32(msg+4, (8u<<16)|5u); /* opcode 5 = done */
    raw_write(c->waybar_fd, &c->write_mu, msg, 8);
}

void emit_closed(struct Client *c, uint32_t obj) {
    uint8_t msg[8];
    wu32(msg+0, obj);
    wu32(msg+4, (8u<<16)|6u); /* opcode 6 = closed */
    raw_write(c->waybar_fd, &c->write_mu, msg, 8);
}

void emit_fake_output_events(struct Client *c) {
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

void emit_fake_xdg_output_events(struct Client *c, uint32_t xdg_output_id) {
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

bool client_is_manager_id(struct Client *c, uint32_t id) {
    for (int i = 0; i < c->manager_n; i++)
        if (c->manager_ids[i] == id) return true;
    return false;
}

bool client_is_layer_surface_id(struct Client *c, uint32_t id) {
    for (int i = 0; i < c->layer_surface_n; i++)
        if (c->layer_surface_ids[i] == id) return true;
    return false;
}

void client_add_layer_surface_id(struct Client *c, uint32_t id) {
    if (client_is_layer_surface_id(c, id)) return;
    if (c->layer_surface_n < 64) c->layer_surface_ids[c->layer_surface_n++] = id;
}

uint32_t client_sid(struct Client *c,
    struct zwlr_foreign_toplevel_handle_v1 *h, uint32_t manager_id) {
    for (int i = 0; i < c->hmap_n; i++)
        if (c->hmap[i].h == h && c->hmap[i].manager_id == manager_id) return c->hmap[i].cid;
    return 0;
}

struct HEntry *client_hentry_for_sid(struct Client *c, uint32_t sid) {
    for (int i = 0; i < c->hmap_n; i++)
        if (c->hmap[i].cid == sid) return &c->hmap[i];
    return NULL;
}

bool client_should_drop_server_id(struct Client *c, uint32_t id) {
    if (client_is_layer_surface_id(c, id)) return false;
    for (int i = 0; i < c->drop_n; i++)
        if (c->drop_ids[i] == id) return true;
    return false;
}

bool client_is_synthetic_handle_id(struct Client *c, uint32_t id) {
    return id >= SERVER_ID_BASE && id < c->next_sid;
}

void client_add_drop_server_id(struct Client *c, uint32_t id) {
    if (client_is_layer_surface_id(c, id)) return;
    if (client_should_drop_server_id(c, id)) return;
    if (c->drop_n < HANDLE_MAP) c->drop_ids[c->drop_n++] = id;
}

bool client_is_registry_id(struct Client *c, uint32_t id) {
    for (int i = 0; i < c->registry_n; i++)
        if (c->registry_ids[i] == id) return true;
    return false;
}

void client_add_registry_id(struct Client *c, uint32_t id) {
    if (client_is_registry_id(c, id)) return;
    if (c->registry_n < 32) c->registry_ids[c->registry_n++] = id;
}

void emit_output_enter_once(struct Client *c, struct HEntry *entry) {
    if (!c->output_id || !c->output_done_received || entry->output_entered) return;
    emit_output_enter(c, entry->cid);
    entry->output_entered = true;
}

void replay_output_enters(struct Client *c) {
    if (!c->output_id) return;
    for (int i = 0; i < c->hmap_n; i++) {
        if (c->hmap[i].output_entered) continue;
        emit_output_enter_once(c, &c->hmap[i]);
        emit_done(c, c->hmap[i].cid);
    }
}

void flush_toplevel(struct Client *c, struct Toplevel *t) {
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
