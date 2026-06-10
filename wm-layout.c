#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <wayland-client.h>

#include "types.h"
#include "wm-log.h"
#include "wm-state.h"
#include "wm-layout.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define BORDER_WIDTH 2

uint32_t chan(uint8_t value) {
	return (uint32_t)value * 0x01010101u;
}

uint32_t all_edges(void) {
	return RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;
}

static void window_apply_borders(struct Window *w, enum BorderState desired) {
	if (w->border_state == desired) return;
	if (desired == BORDER_NONE) {
		river_window_v1_set_borders(w->obj, RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
	} else if (desired == BORDER_TILED) {
		river_window_v1_set_borders(w->obj, all_edges(), BORDER_WIDTH, 0, 0, 0, 0x00000000u);
	}
	w->border_state = desired;
}

static void window_set_visible(struct Window *w, bool visible) {
	if (w->applied_visible == visible) return;
	if (visible) {
		river_window_v1_show(w->obj);
	} else {
		river_window_v1_hide(w->obj);
	}
	w->applied_visible = visible;
}

size_t window_count(void) {
	size_t count = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window->parent == NULL && !window->floating) count++;
	}
	return count;
}

size_t visible_window_count(void) {
	size_t count = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (!window->minimized && window->parent == NULL && !window->floating) count++;
	}
	return count;
}

static struct wl_list *visible_region_end(void) {
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window->minimized || window->parent != NULL || window->floating) return &window->link;
	}
	return &wm.windows;
}

struct Window *window_at(size_t index) {
	size_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window->parent != NULL || window->floating) continue;
		if (i == index) return window;
		i++;
	}
	return NULL;
}

int32_t window_index(struct Window *needle) {
	int32_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window->parent != NULL || window->floating) continue;
		if (window == needle) return i;
		i++;
	}
	return -1;
}

struct Window *target_window(void) {
	if (wm.target_index >= visible_window_count()) return NULL;
	return window_at(wm.target_index);
}

void clamp_target(void) {
	size_t count = visible_window_count();
	if (count == 0) {
		wm.target_index = 0;
		wm.maximized = false;
		return;
	}
	if (wm.target_index > 1 || wm.target_index >= count) {
		wm.target_index = 0;
	}
}

struct Output *active_output(void) {
	struct Output *fallback = NULL;
	struct Output *output;
	wl_list_for_each(output, &wm.outputs, link) {
		if (output->removed) continue;
		if (fallback == NULL) fallback = output;
		if (output->width > 0 && output->height > 0) return output;
	}
	return fallback;
}

struct Box output_box(void) {
	struct Output *output = active_output();
	if (output == NULL || output->width <= 0 || output->height <= 0) {
		return (struct Box){ .x = 0, .y = 0, .width = DEFAULT_WIDTH, .height = DEFAULT_HEIGHT };
	}
	if (output->usable_width > 0 && output->usable_height > 0) {
		return (struct Box){
			.x = output->usable_x, .y = output->usable_y,
			.width = output->usable_width, .height = output->usable_height,
		};
	}
	return (struct Box){ .x = output->x, .y = output->y, .width = output->width, .height = output->height };
}

static struct Box layout_box_for_index(size_t index) {
	struct Box out = output_box();
	size_t count = visible_window_count();
	if (count <= 1 || wm.maximized) {
		return out;
	}

	int32_t main_width = (out.width * 2) / 3;
	if (main_width < 1) main_width = out.width;
	int32_t deck_width = out.width - main_width;
	if (deck_width < 1) deck_width = 1;

	if (index == 0) {
		return (struct Box){ .x = out.x, .y = out.y, .width = main_width, .height = out.height };
	}
	return (struct Box){ .x = out.x + main_width, .y = out.y, .width = deck_width, .height = out.height };
}

static bool window_is_visible_index(size_t index) {
	if (wm.maximized) return index == wm.target_index;
	return index < 2 && index < visible_window_count();
}

static void move_after(struct Window *window, struct wl_list *position) {
	wl_list_remove(&window->link);
	wl_list_insert(position, &window->link);
}

void move_first(struct Window *window) {
	move_after(window, &wm.windows);
}

void move_last(struct Window *window) {
	if (wm.windows.prev == &window->link) return; // já é a cauda
	move_after(window, wm.windows.prev);
}

void spawn_command(const char *cmd) {
	if (fork() == 0) {
		execlp(cmd, cmd, (char *)0);
		_exit(127);
	}
}

void spawn_sh(const char *cmd) {
	if (fork() == 0) {
		execlp("sh", "sh", "-c", cmd, (char *)0);
		_exit(127);
	}
}

void window_set_string(char **field, const char *value) {
	free(*field);
	*field = value == NULL ? NULL : strdup(value);
}

bool valid_identifier(const char *id) {
	if (id == NULL || id[0] == '\0') return false;
	size_t len = strlen(id);
	if (len > 32) return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)id[i];
		if (c < 0x21 || c > 0x7e) return false;
	}
	return true;
}

static struct Window *window_by_identifier(const char *id) {
	if (!valid_identifier(id)) return NULL;
	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		if (w->closed || w->identifier == NULL) continue;
		if (strcmp(w->identifier, id) == 0) return w;
	}
	return NULL;
}

static void activate_window_from_taskbar(struct Window *window) {
	int32_t idx = window_index(window);
	if (idx < 0) return;
	if (window->minimized) {
		window->minimized = false;
		move_first(window);
		wm.target_index = 0;
	} else if (idx == 0 || idx == 1) {
		wm.target_index = (uint32_t)idx;
	} else {
		move_first(window);
		wm.target_index = 0;
	}
	wm.maximized = false;
	LOG_EVENT("taskbar activate: idx=%d app_id=%s identifier=%s",
		idx,
		window->app_id ? window->app_id : "",
		window->identifier ? window->identifier : "");
}

void apply_pending_taskbar_activation(void) {
	if (pending_activate_identifier[0] == '\0') return;
	char id[33];
	snprintf(id, sizeof(id), "%s", pending_activate_identifier);
	pending_activate_identifier[0] = '\0';
	struct Window *window = window_by_identifier(id);
	if (window == NULL) {
		LOG_WARN("taskbar activate: identifier desconhecido=%s (no-op)", id);
		return;
	}
	activate_window_from_taskbar(window);
}

static void osd(const char *message) {
	LOG_INFO("OSD: %s", message);
	if (fork() == 0) {
		// x-canonical-private-synchronous: notificações com o mesmo valor de
		// hint se substituem no lugar em vez de empilhar (suportado pelo mako).
		// Sem isso, cada comando gera um OSD novo e eles acumulam — e como não
		// há config do mako, o default-timeout dele é 0 (nunca expira sozinho),
		// então os antigos só sairiam pelo expire-time, ficando empilhados.
		execlp("notify-send", "notify-send",
			"--expire-time=1200",
			"--urgency=low",
			"--app-name=maindeck",
			"--hint=string:x-canonical-private-synchronous:maindeck-osd",
			message, (char *)0);
		_exit(127);
	}
}

void focus_target_on_seats(void) {
	clamp_target();
	struct Window *target = target_window();
	struct Window *focus = target;
	if (target != NULL) {
		struct Window *w;
		wl_list_for_each(w, &wm.windows, link) {
			if (w->parent == NULL || !window_is_really_visible(w)) continue;
			struct Window *root = w->parent;
			int depth = 0;
			while (root->parent != NULL && ++depth < 32) root = root->parent;
			if (root == target) focus = w;
		}
	}
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->removed) continue;
		if (seat->focused != NULL && seat->focused->floating && !seat->focused->closed && !wm.focus_dirty) {
			continue;
		}
		if (focus != NULL) {
			river_seat_v1_clear_focus(seat->obj);
			river_seat_v1_focus_window(seat->obj, focus->obj);
		} else {
			river_seat_v1_clear_focus(seat->obj);
		}
		seat->focused = focus;
	}
}

void log_state(void) {
	if (!md_verbose()) return;

	size_t count = window_count();
	LOG_STATE("windows=%zu target=%u maximized=%d", count, wm.target_index, wm.maximized);
	size_t i = 0;
	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		if (w->parent != NULL) {
			LOG_STATE("  [CHILD] \"%s\" parent=\"%s\" app_id=%s size=%dx%d implicit=%d",
				w->title ? w->title : "",
				w->parent->title ? w->parent->title : "",
				w->app_id ? w->app_id : "",
				w->width,
				w->height,
				w->implicit_parent ? 1 : 0);
		} else if (w->floating) {
			LOG_STATE("  [FLOAT] \"%s\" app_id=%s size=%dx%d",
				w->title ? w->title : "",
				w->app_id ? w->app_id : "",
				w->width,
				w->height);
		} else {
			const char *role = i == 0 ? "MAIN" : (i == 1 ? "DECK" : "hidden");
			const char *target = (i == wm.target_index) ? " [ALVO]" : "";
			LOG_STATE("  [%zu] %s \"%s\" app_id=%s%s", i, role,
				w->title ? w->title : "", w->app_id ? w->app_id : "", target);
			i++;
		}
	}
}

void md_swap_main_deck(void) {
	if (visible_window_count() < 2) return;
	struct Window *main = window_at(0);
	struct Window *deck = window_at(1);
	if (main == NULL || deck == NULL) return;
	move_first(deck);
	wm.target_index = wm.target_index == 0 ? 1 : 0;
	wm.maximized = false;
	log_state();
}

void md_deck_next(void) {
	if (visible_window_count() <= 2) {
		osd("sem janela invis\xc3\xadvel \xc3\xa0 direita");
		return;
	}
	struct Window *deck = window_at(1);
	wl_list_remove(&deck->link);
	wl_list_insert(visible_region_end()->prev, &deck->link);
	wm.target_index = 1;
	wm.maximized = false;
	log_state();
}

void md_deck_prev(void) {
	size_t vis = visible_window_count();
	if (vis <= 2) {
		osd("sem janela invis\xc3\xadvel \xc3\xa0 esquerda");
		return;
	}
	struct Window *last_visible = window_at(vis - 1);
	struct Window *main = window_at(0);
	move_after(last_visible, &main->link);
	wm.target_index = 1;
	wm.maximized = false;
	log_state();
}

void md_send_target_to_deck_bottom(void) {
	size_t count = visible_window_count();
	if (count <= 1) return;
	struct Window *target = target_window();
	if (target == NULL) return;
	if (wm.target_index == 1 && count <= 2) return;

	if (wm.target_index == 0) {
		// MAIN → move to DECK visible (index 1), ALVO follows the window there.
		// Old DECK slides up to MAIN; no hidden windows are affected.
		struct Window *deck = window_at(1);
		wl_list_remove(&target->link);
		wl_list_insert(&deck->link, &target->link); // deck=0, target=1
		wm.target_index = 1;
	} else {
		// DECK → send to hidden bottom as before; next hidden window surfaces to DECK.
		wl_list_remove(&target->link);
		wl_list_insert(visible_region_end()->prev, &target->link);
		clamp_target();
	}

	wm.maximized = false;
	log_state();
}

void md_promote_target_to_main(void) {
	struct Window *target = target_window();
	if (target == NULL || wm.target_index == 0) return;
	move_first(target);
	wm.target_index = 0;
	wm.maximized = false;
	log_state();
}

static int32_t clamp_implicit_child_dimension(int32_t value, int32_t min, int32_t cap) {
	if (min > 0 && value < min) value = min;
	if (cap > 0 && value > cap) value = cap;
	return value > 0 ? value : 1;
}

static struct Window *root_window(struct Window *window) {
	struct Window *root = window;
	int depth = 0;
	while (root != NULL && root->parent != NULL && ++depth < 32) {
		root = root->parent;
	}
	return root;
}

static int32_t natural_implicit_child_dimension(int32_t current, int32_t min, int32_t max, int32_t cap, int32_t fallback) {
	if (max > 0 && max <= cap) return max;
	if (min > 0 && min <= cap) return min;
	if (current > 0 && current <= cap) return current;
	return fallback;
}

static void child_proposed_dimensions(struct Window *window, int32_t *width, int32_t *height) {
	if (!window->implicit_parent) {
		*width = 0;
		*height = 0;
		return;
	}

	struct Window *root = root_window(window);
	int32_t pidx = root != NULL ? window_index(root) : -1;
	struct Box pbox = pidx >= 0 ? layout_box_for_index((size_t)pidx) : output_box();

	int32_t max_dialog_w = (pbox.width * 3) / 4;
	int32_t max_dialog_h = (pbox.height * 3) / 4;
	if (max_dialog_w < 1) max_dialog_w = DEFAULT_WIDTH;
	if (max_dialog_h < 1) max_dialog_h = DEFAULT_HEIGHT;

	int32_t fallback_w = max_dialog_w < 480 ? max_dialog_w : 480;
	int32_t fallback_h = max_dialog_h < 320 ? max_dialog_h : 320;
	int32_t natural_w = natural_implicit_child_dimension(window->width, window->min_width, window->max_width, max_dialog_w, fallback_w);
	int32_t natural_h = natural_implicit_child_dimension(window->height, window->min_height, window->max_height, max_dialog_h, fallback_h);

	int32_t proposed_w = natural_w + 48;
	int32_t proposed_h = natural_h + 40;

	*width = clamp_implicit_child_dimension(proposed_w, window->min_width, max_dialog_w);
	*height = clamp_implicit_child_dimension(proposed_h, window->min_height, max_dialog_h);
}

void md_insert_new_window(struct Window *window) {
	wm.last_placed_top_node = NULL;
	size_t count = window_count();
	LOG_EVENT("new window: count_before=%zu", count);
	if (count == 0) {
		// First window: just MAIN.
		wl_list_insert(wm.windows.prev, &window->link);
	} else {
		// Stack behavior: the new window becomes MAIN (index 0), the previous
		// MAIN slides to the visible DECK (index 1), the previous visible DECK
		// is pushed back to hidden (index 2), and so on — like a stack where new
		// windows always come out on top. Inserting at the head of the list does
		// exactly this, since the list is already in MainDeck order.
		wl_list_insert(&wm.windows, &window->link);
	}
	wm.target_index = 0;
	wm.maximized = false;
	wm.focus_dirty = true;
	log_state();
}

void window_manage_layout(struct Window *window, size_t index) {
	// Transient windows / dialogs: compositor/client owns position/dimensions.
	if (window->parent != NULL) {
		river_window_v1_use_ssd(window->obj);
		river_window_v1_set_tiled(window->obj, RIVER_WINDOW_V1_EDGES_NONE);
		if (!window->transient_size_proposed) {
			int32_t width, height;
			child_proposed_dimensions(window, &width, &height);
			river_window_v1_propose_dimensions(window->obj, width, height);
			LOG_EVENT("child dimensions proposed: \"%s\" implicit=%d proposed=%dx%d hint=%dx%d..%dx%d",
				window->title ? window->title : "",
				window->implicit_parent ? 1 : 0,
				width,
				height,
				window->min_width,
				window->min_height,
				window->max_width,
				window->max_height);
			window->transient_size_proposed = true;
		}
		window->new = false;
		return;
	}

	if (window->floating) {
		river_window_v1_use_ssd(window->obj);
		river_window_v1_set_tiled(window->obj, RIVER_WINDOW_V1_EDGES_NONE);
		window->new = false;
		return;
	}

	// Client-requested fullscreen: the compositor owns position/dimensions, so
	// we must NOT propose_dimensions/set_tiled. Issue fullscreen()/exit on the
	// transition edge (both are manage-sequence-only requests).
	// NOTE: a minimized window must NOT enter fullscreen (it's force-hidden in
	// render); the !minimized guard lets a minimize-from-fullscreen fall through
	// to the exit-reconcile block below so the compositor is told exit_fullscreen.
	if (window->fullscreen && !window->minimized) {
		if (!window->applied_fullscreen) {
			struct river_output_v1 *out = window->fs_output;
			if (out == NULL) {
				struct Output *o = active_output();
				out = o ? o->obj : NULL;
			}
			if (out != NULL) {
				river_window_v1_fullscreen(window->obj, out);
				river_window_v1_inform_fullscreen(window->obj); // tell the client too
				window->applied_fullscreen = true;
			}
		}
		window->new = false;
		return;
	}
	if (window->applied_fullscreen) {
		// Leaving fullscreen: exit + re-propose dimensions this same sequence.
		// This also runs when a fullscreen window was just minimized (above guard
		// skipped the apply block), so the compositor is told to exit fullscreen
		// instead of leaving applied_fullscreen orphaned while hidden.
		river_window_v1_exit_fullscreen(window->obj);
		river_window_v1_inform_not_fullscreen(window->obj);
		window->applied_fullscreen = false;
	}

	// Minimized: force-hidden in render; nothing to size/tile here. Checked AFTER
	// the fullscreen exit-reconcile so minimize-from-fullscreen still issues exit.
	if (window->minimized) { window->new = false; return; }

	if (!window_is_visible_index(index)) return;
	struct Box box = layout_box_for_index(index);
	int32_t width = box.width - (BORDER_WIDTH * 2);
	int32_t height = box.height - (BORDER_WIDTH * 2);
	river_window_v1_use_ssd(window->obj);
	river_window_v1_set_tiled(window->obj, all_edges());
	river_window_v1_propose_dimensions(window->obj, width > 1 ? width : 1, height > 1 ? height : 1);
	window->new = false;
}

void window_render_layout(struct Window *window, size_t index) {
	if (window->minimized) {
		window_set_visible(window, false);
		window_apply_borders(window, BORDER_NONE);
		return;
	}
	// Transient windows / dialogs: float centered above parent, no WM borders.
	if (window->parent != NULL) {
		bool visible = window_is_really_visible(window);
		window_set_visible(window, visible);
		if (visible) {
			// Center the child over the parent's layout box.
			struct Window *root = root_window(window);
			int32_t pidx = window_index(root);
			if (pidx >= 0) {
				struct Box pbox = layout_box_for_index((size_t)pidx);
				int32_t cw = window->width  > 0 ? window->width  : pbox.width  / 2;
				int32_t ch = window->height > 0 ? window->height : pbox.height / 2;
				int32_t cx = pbox.x + (pbox.width  - cw) / 2;
				int32_t cy = pbox.y + (pbox.height - ch) / 2;
				river_node_v1_set_position(window->node, cx, cy);
			}
			wm_place_top(window->node);
		}
		window_apply_borders(window, BORDER_NONE);
		return;
	}
	if (window->floating) {
		window_set_visible(window, true);
		window_apply_borders(window, BORDER_NONE);
		wm_place_top(window->node);
		return;
	}
	// Fullscreen window: compositor owns geometry; just show it, no borders,
	// node on top (so it's the top fullscreen window — anything above it in the
	// render order, like waybar, keeps drawing; see place_top handling).
	if (window->fullscreen) {
		window_set_visible(window, true);
		window_apply_borders(window, BORDER_NONE);
		wm_place_top(window->node);
		return;
	}

	if (!window_is_visible_index(index)) {
		window_set_visible(window, false);
		window_apply_borders(window, BORDER_NONE);
		return;
	}

	struct Box box = layout_box_for_index(index);
	window_set_visible(window, true);
	river_node_v1_set_position(window->node, box.x + BORDER_WIDTH, box.y + BORDER_WIDTH);
	wm_place_top(window->node);

	// Borda transparente para todas as janelas (focado e desfocado).
	// Mantemos a largura (BORDER_WIDTH) com alpha 0 para que a geometria e
	// o posicionamento das janelas permaneçam idênticos.
	window_apply_borders(window, BORDER_TILED);
}

void wm_place_top(struct river_node_v1 *node) {
	if (wm.last_placed_top_node != node) {
		river_node_v1_place_top(node);
		wm.last_placed_top_node = node;
	}
}

bool window_is_really_visible(struct Window *w) {
	int depth = 0;
	while (w != NULL) {
		if (w->minimized) return false;
		if (w->parent == NULL) break;
		if (++depth > 32) return false;   // guarda anti-ciclo / aninhamento patológico
		w = w->parent;
	}
	if (w == NULL) return false;
	int32_t idx = window_index(w);
	if (idx < 0) return false;
	return window_is_visible_index((size_t)idx);
}
