#ifndef WM_LAYOUT_H
#define WM_LAYOUT_H

#include <stddef.h>
#include <stdbool.h>
#include "types.h"

size_t window_count(void);
size_t visible_window_count(void);
struct Window *window_at(size_t index);
int32_t window_index(struct Window *needle);
struct Window *target_window(void);
void clamp_target(void);
struct Output *active_output(void);
struct Box output_box(void);
void spawn_command(const char *cmd);
void spawn_sh(const char *cmd);
void window_set_string(char **field, const char *value);
bool valid_identifier(const char *id);
void apply_pending_taskbar_activation(void);
void focus_target_on_seats(void);
void log_state(void);
void md_swap_main_deck(void);
void md_deck_next(void);
void md_deck_prev(void);
void md_send_target_to_deck_bottom(void);
void md_promote_target_to_main(void);
void md_insert_new_window(struct Window *window);
void window_manage_layout(struct Window *window, size_t index);
void window_render_layout(struct Window *window, size_t index);
void move_first(struct Window *window);
void move_last(struct Window *window);
void wm_place_top(struct river_node_v1 *node);
struct Window *window_by_obj(struct river_window_v1 *obj);

#endif /* WM_LAYOUT_H */
