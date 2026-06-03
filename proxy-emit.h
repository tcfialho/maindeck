#ifndef PROXY_EMIT_H
#define PROXY_EMIT_H

#include "proxy-types.h"

uint32_t emit_toplevel_new(struct Client *c, struct zwlr_foreign_toplevel_handle_v1 *h, uint32_t manager_id);
void emit_str(struct Client *c, uint32_t obj, uint16_t op, const char *s);
void emit_state(struct Client *c, uint32_t obj, bool activated);
void emit_output_enter(struct Client *c, uint32_t obj);
void emit_done(struct Client *c, uint32_t obj);
void emit_closed(struct Client *c, uint32_t obj);
void emit_fake_output_events(struct Client *c);
void emit_fake_xdg_output_events(struct Client *c, uint32_t xdg_output_id);
void emit_output_enter_once(struct Client *c, struct HEntry *entry);

bool client_is_manager_id(struct Client *c, uint32_t id);
bool client_is_layer_surface_id(struct Client *c, uint32_t id);
void client_add_layer_surface_id(struct Client *c, uint32_t id);
uint32_t client_sid(struct Client *c, struct zwlr_foreign_toplevel_handle_v1 *h, uint32_t manager_id);
struct HEntry *client_hentry_for_sid(struct Client *c, uint32_t sid);
bool client_should_drop_server_id(struct Client *c, uint32_t id);
bool client_is_synthetic_handle_id(struct Client *c, uint32_t id);
void client_add_drop_server_id(struct Client *c, uint32_t id);
bool client_is_registry_id(struct Client *c, uint32_t id);
void client_add_registry_id(struct Client *c, uint32_t id);

void replay_output_enters(struct Client *c);
void flush_toplevel(struct Client *c, struct Toplevel *t);

#endif /* PROXY_EMIT_H */
