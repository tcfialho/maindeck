#ifndef WM_LAYOUT_H
#define WM_LAYOUT_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include "types.h"

struct LayoutView {
	struct Box output;
	int visible_count;
	struct Window *target;
	struct Window *main_win;
	struct Window *deck_win;
	bool single;
	bool maximized;
	int32_t target_index;
	struct Box main_box;
	struct Box deck_box;
};

void layout_view_init(struct LayoutView *view);
size_t window_count(void);
size_t visible_window_count(void);
int32_t window_index(struct Window *needle);
struct Window *target_window(void);
void clamp_target(void);
struct Output *active_output(void);
struct Box output_box(void);
pid_t spawn_command(const char *cmd);
void spawn_sh(const char *cmd);
void window_set_string(char **field, const char *value);
bool valid_identifier(const char *id);
void apply_pending_taskbar_activation(void);
/* Aplica a ação do menu de contexto da barra (pending_window_action sobre
   pending_window_action_id). Chamada dentro do manage cycle. No-op se nada
   pendente ou identifier desconhecido. */
void apply_pending_window_action(void);
void focus_target_on_seats(void);
void log_state(void);
void md_swap_main_deck(void);
void md_deck_next(void);
void md_deck_prev(void);
void md_send_target_to_deck_bottom(void);
void md_promote_target_to_main(void);
/* Minimize a specific window (used by both the protocol minimize_requested
   handler and the keybinding). Sends it to the tail and force-hides it. */
void md_minimize_window(struct Window *window);
/* Context-menu (taskbar right-click) actions on a specific window. */
void md_maximize_window(struct Window *window);
void md_restore_window(struct Window *window);
/* Minimize the currently targeted window (keybinding action). */
void md_minimize_target(void);
/* Un-minimize the most recently minimized window (LIFO), bringing it back as
   MAIN. No-op if nothing is minimized. */
void md_unminimize(void);
void md_insert_new_window(struct Window *window);
void window_manage_layout(struct Window *window, size_t index, const struct LayoutView *view);
void window_render_layout(struct Window *window, size_t index, const struct LayoutView *view);
void move_first(struct Window *window);
void move_last(struct Window *window);
void wm_place_top(struct river_node_v1 *node);
bool window_is_really_visible(struct Window *w);
bool window_is_really_visible_view(struct Window *w, const struct LayoutView *view);
#endif /* WM_LAYOUT_H */
