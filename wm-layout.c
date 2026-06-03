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
#define BORDER_WIDTH 3

uint32_t chan(uint8_t value) {
	return (uint32_t)value * 0x01010101u;
}

uint32_t all_edges(void) {
	return RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;
}

size_t window_count(void) {
	size_t count = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		count++;
	}
	return count;
}

size_t visible_window_count(void) {
	size_t count = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (!window->minimized) count++;
	}
	return count;
}

static struct wl_list *visible_region_end(void) {
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window->minimized) return &window->link;
	}
	return &wm.windows;
}

struct Window *window_at(size_t index) {
	size_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (i == index) return window;
		i++;
	}
	return NULL;
}

int32_t window_index(struct Window *needle) {
	int32_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
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
	struct Seat *seat;
	wl_list_for_each(seat, &wm.seats, link) {
		if (seat->removed) continue;
		if (target != NULL) {
			river_seat_v1_focus_window(seat->obj, target->obj);
		} else {
			river_seat_v1_clear_focus(seat->obj);
		}
		seat->focused = target;
	}
}

void log_state(void) {
	size_t count = window_count();
	LOG_STATE("windows=%zu target=%u maximized=%d", count, wm.target_index, wm.maximized);
	size_t i = 0;
	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		const char *role = i == 0 ? "MAIN" : (i == 1 ? "DECK" : "hidden");
		const char *target = (i == wm.target_index) ? " [ALVO]" : "";
		LOG_STATE("  [%zu] %s \"%s\" app_id=%s%s", i, role,
			w->title ? w->title : "", w->app_id ? w->app_id : "", target);
		i++;
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

void md_insert_new_window(struct Window *window) {
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
	log_state();
}

void window_manage_layout(struct Window *window, size_t index) {
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
		river_window_v1_hide(window->obj);
		river_window_v1_set_borders(window->obj, RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
		return;
	}
	// Fullscreen window: compositor owns geometry; just show it, no borders,
	// node on top (so it's the top fullscreen window — anything above it in the
	// render order, like waybar, keeps drawing; see place_top handling).
	if (window->fullscreen) {
		river_window_v1_show(window->obj);
		river_window_v1_set_borders(window->obj, RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
		river_node_v1_place_top(window->node);
		return;
	}

	if (!window_is_visible_index(index)) {
		river_window_v1_hide(window->obj);
		river_window_v1_set_borders(window->obj, RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
		return;
	}

	struct Box box = layout_box_for_index(index);
	river_window_v1_show(window->obj);
	river_node_v1_set_position(window->node, box.x + BORDER_WIDTH, box.y + BORDER_WIDTH);
	river_node_v1_place_top(window->node);

	if (index == wm.target_index) {
		// ALVO (focado): borda amarela.
		river_window_v1_set_borders(window->obj, all_edges(), BORDER_WIDTH,
			chan(245), chan(197), chan(66), 0xffffffffu);
	} else {
		// Sem foco (MAIN ou DECK): borda transparente. Mantemos a MESMA largura
		// (BORDER_WIDTH) com alpha 0 em vez de largura 0, pois a geometria das
		// janelas já é insetada por BORDER_WIDTH (window_apply_dimensions e o
		// set_position com +BORDER_WIDTH). Largura 0 deixaria um buraco de 3px;
		// alpha 0 some com a borda sem mexer em posição/tamanho.
		river_window_v1_set_borders(window->obj, all_edges(), BORDER_WIDTH,
			0, 0, 0, 0x00000000u);
	}
}
