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
#include "wm-animation-intents.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define BORDER_WIDTH 3
#define GAP 12

uint32_t chan(uint8_t value) {
	return (uint32_t)value * 0x01010101u;
}

uint32_t all_edges(void) {
	return RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT;
}

static void window_apply_borders(struct Window *w, enum BorderState desired) {
	bool is_target = (w == target_window());
	if (w->border_state == desired && w->border_focused == is_target) return;

	if (desired == BORDER_NONE) {
		river_window_v1_set_borders(w->obj, RIVER_WINDOW_V1_EDGES_NONE, 0, 0, 0, 0, 0);
	} else if (desired == BORDER_TILED) {
		if (is_target) {
			// Apenas traço azul na parte de baixo (2px de espessura): #2563eb.
			// NOTA: RIVER_WINDOW_V1_EDGES_BOTTOM (2) é mapeado via @bitCast em Zig 
			// para o segundo campo da packed struct Edges {top, bottom, left, right},
			// resultando em top=false e bottom=true. A borda superior fica estritamente inativa.
			river_window_v1_set_borders(w->obj, RIVER_WINDOW_V1_EDGES_BOTTOM, 2,
				chan(37), chan(99), chan(235), chan(255));
		} else {
			// Sem bordas nas janelas inativas
			river_window_v1_set_borders(w->obj, RIVER_WINDOW_V1_EDGES_NONE, 0,
				0, 0, 0, 0x00000000u);
		}
	}
	w->border_state = desired;
	w->border_focused = is_target;
}

static void window_set_visible(struct Window *w, bool visible) {
	if (w->applied_visible == visible) return;
	if (visible) {
		river_window_v1_show(w->obj);
	} else {
		LOG_EVENT("[VIS-DIAG] hide SENT win=\"%s\" minimized=%d", w->title ? w->title : "", w->minimized);
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

static struct Window *window_at(size_t index) {
	size_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window->parent != NULL || window->floating) continue;
		if (i == index) return window;
		i++;
	}
	return NULL;
}

// Marca PER-WINDOW (pending_anim) todas as janelas tiled atualmente VISÍVEIS
// (não-minimizadas, nos 2 primeiros slots). Usado por ações que mexem na
// geometria de várias janelas ao mesmo tempo com o MESMO efeito (maximize/
// restore, promote, send-to-deck-bottom): cada janela afetada carrega seu próprio
// intent, eliminando o pending_anim global. `intent` é um AnimationIntent (passado
// como uint32_t p/ não acoplar este header ao de animação). Chamar DEPOIS da
// mutação da lista.
void mark_visible_tiled_anim(uint32_t intent) {
	size_t i = 0;
	struct Window *window;
	wl_list_for_each(window, &wm.windows, link) {
		if (window->minimized || window->parent != NULL || window->floating) continue;
		if (i >= 2) break; // só os 2 slots visíveis (MAIN + DECK)
		window->pending_anim = intent;
		i++;
	}
}

// Após um CLOSE (cliente fechou a janela e ela já saiu da lista), se sobrou
// EXATAMENTE UMA janela tiled visível, marca-a com GROW_REVEAL: ela vai crescer
// p/ preencher o espaço da que fechou, via clip crescente (P15 grow-reveal). É a
// declaração que faltava p/ o close iniciado pelo cliente — ninguém "agiu" sobre
// a sobrevivente (o cliente fechou sozinho), então o WM a marca aqui, no destroy.
// Só dispara p/ lone survivor (sobra 1); com 2+ as sobreviventes só refluem.
void md_mark_grow_survivor_if_lone(void) {
	if (visible_window_count() != 1) return;
	struct Window *survivor = window_at(0);
	if (survivor != NULL) {
		survivor->pending_anim = ANIMATION_INTENT_GROW_REVEAL;
	}
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

void layout_view_init(struct LayoutView *view) {
	view->output = output_box();
	view->visible_count = 0;
	view->target = target_window();
	view->main_win = NULL;
	view->deck_win = NULL;
	view->maximized = wm.maximized;
	view->target_index = wm.target_index;

	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		if (w->minimized || w->parent != NULL || w->floating) continue;
		if (view->visible_count == 0) {
			view->main_win = w;
		} else if (view->visible_count == 1) {
			view->deck_win = w;
		}
		view->visible_count++;
	}

	view->single = (view->visible_count <= 1 || view->maximized);

	int32_t main_width = (view->output.width * 2) / 3;
	if (main_width < 1) main_width = view->output.width;
	int32_t deck_width = view->output.width - main_width;
	if (deck_width < 1) deck_width = 1;

	// main_box & deck_box
	if (view->single) {
		view->main_box = view->output;
		view->deck_box = view->output;
	} else {
		int32_t w_main = main_width - (GAP / 2);
		view->main_box = (struct Box){
			.x = view->output.x,
			.y = view->output.y,
			.width = w_main > 0 ? w_main : 1,
			.height = view->output.height
		};
		int32_t w_deck = deck_width - (GAP / 2);
		view->deck_box = (struct Box){
			.x = view->output.x + main_width + (GAP / 2),
			.y = view->output.y,
			.width = w_deck > 0 ? w_deck : 1,
			.height = view->output.height
		};
	}
}

static struct Box layout_box_for_index(const struct LayoutView *view, size_t index) {
	if (view->single) return view->output;
	if (index == 0) return view->main_box;
	return view->deck_box;
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

pid_t spawn_command(const char *cmd) {
	pid_t pid = fork();
	if (pid == 0) {
		execlp(cmd, cmd, (char *)0);
		_exit(127);
	}
	return pid;
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
	// Flutuantes não têm índice MainDeck (window_index = -1): ativa por foco.
	if (window->floating) {
		window->minimized = false;
		move_last(window); // topo entre as flutuantes + muda a assinatura → re-render
		struct Seat *seat;
		wl_list_for_each(seat, &wm.seats, link) {
			if (seat->removed) continue;
			river_seat_v1_clear_focus(seat->obj);
			river_seat_v1_focus_window(seat->obj, window->obj);
			seat->focused = window;
		}
		LOG_EVENT("taskbar activate floating: app_id=%s identifier=%s",
			window->app_id ? window->app_id : "",
			window->identifier ? window->identifier : "");
		return;
	}
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

/* Torna `window` o alvo do MainDeck (mesma lógica de targeting do activate
 * tiled), SEM mexer em wm.maximized — o chamador decide. Flutuantes não têm
 * índice MainDeck e não participam do maximize global. */
static void target_tiled_window(struct Window *window) {
	if (window->floating) return;
	int32_t idx = window_index(window);
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
}

/* Menu de contexto: maximiza uma janela específica = foca + maximized global
 * (decisão de design: maximized é estado global do MainDeck, segue o alvo). */
void md_maximize_window(struct Window *window) {
	if (window == NULL || window->closed || window->floating) return;
	target_tiled_window(window);
	wm.maximized = true;
	wm.focus_dirty = true;
	LOG_EVENT("ctx-menu maximize: \"%s\"", window->title ? window->title : "");
	log_state();
}

/* Menu de contexto: "Restaurar" estilo Windows — reverte minimizado OU
 * maximizado. Minimizado tem precedência (volta como MAIN). */
void md_restore_window(struct Window *window) {
	if (window == NULL || window->closed) return;
	if (window->minimized) {
		window->minimized = false;
		move_first(window);
		wm.target_index = 0;
		wm.maximized = false;
		wm.focus_dirty = true;
		LOG_EVENT("ctx-menu restore (unminimize): \"%s\"", window->title ? window->title : "");
		log_state();
		return;
	}
	/* Não-minimizada: só faz sentido sair do maximize, e maximize é global e
	 * segue o alvo — restaura se esta é a janela alvo e estamos maximizados. */
	if (wm.maximized && window == target_window()) {
		wm.maximized = false;
		wm.focus_dirty = true;
		LOG_EVENT("ctx-menu restore (unmaximize): \"%s\"", window->title ? window->title : "");
		log_state();
	}
}

void apply_pending_window_action(void) {
	if (pending_window_action == WINDOW_ACTION_NONE) return;
	enum WindowAction action = pending_window_action;
	char id[33];
	snprintf(id, sizeof(id), "%s", pending_window_action_id);
	pending_window_action = WINDOW_ACTION_NONE;
	pending_window_action_id[0] = '\0';

	struct Window *window = window_by_identifier(id);
	if (window == NULL) {
		LOG_WARN("ctx-menu: identifier desconhecido=%s (no-op)", id);
		return;
	}
	switch (action) {
	case WINDOW_ACTION_MINIMIZE:
		md_minimize_window(window);
		wm.focus_dirty = true;
		LOG_EVENT("ctx-menu minimize: \"%s\"", window->title ? window->title : "");
		log_state();
		break;
	case WINDOW_ACTION_MAXIMIZE:
		md_maximize_window(window);
		break;
	case WINDOW_ACTION_RESTORE:
		md_restore_window(window);
		break;
	case WINDOW_ACTION_NONE:
		break;
	}
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
		} else if (w->minimized) {
			LOG_STATE("  [MIN] \"%s\" app_id=%s", w->title ? w->title : "",
				w->app_id ? w->app_id : "");
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
	// DECLARATIVO: as DUAS janelas trocam de slot/tamanho — ambas refluem.
	// P17 removeu o spring do swap (usa `ease-in-out 0.20s`, igual ao reflow);
	// marca REFLOW_EASE per-window → o river arma armMove(.ease_in_out 200ms) por
	// declaração, não por inferência de geometria.
	main->pending_anim = ANIMATION_INTENT_REFLOW_EASE;
	deck->pending_anim = ANIMATION_INTENT_REFLOW_EASE;
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
	// DECK_NEXT (Win+→): a sequência segue a seta — a janela do deck SAI pela
	// DIREITA (SLIDE_DECK_OUT) e a próxima oculta ENTRA pela ESQUERDA (DECK_IN_LEFT,
	// com clip). Marca per-window os dois papéis no momento da ação (semântico, sem
	// inferência geométrica nem pending_anim global ambíguo). DECK_IN_LEFT é
	// distinto de SLIDE_IN (que ficou só para o open-em-grupo que vira main) — o
	// river decide a direção pelo enum, não por box.x.
	deck->pending_anim = ANIMATION_INTENT_SLIDE_DECK_OUT; // sai p/ direita
	wl_list_remove(&deck->link);
	wl_list_insert(visible_region_end()->prev, &deck->link);
	struct Window *new_deck = window_at(1); // a que subiu p/ o slot do deck
	if (new_deck != NULL) {
		new_deck->pending_anim = ANIMATION_INTENT_DECK_IN_LEFT; // entra pela esquerda
	}
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
	struct Window *old_deck = window_at(1); // será empurrada p/ hidden
	// DECK_PREV (Win+←): espelho do NEXT — a janela do deck SAI pela ESQUERDA
	// (SLIDE_DECK_OUT_LEFT) e a que entra vem da DIREITA (DECK_IN_RIGHT).
	if (old_deck != NULL) {
		old_deck->pending_anim = ANIMATION_INTENT_SLIDE_DECK_OUT_LEFT; // sai p/ esquerda
	}
	last_visible->pending_anim = ANIMATION_INTENT_DECK_IN_RIGHT; // entra pela direita
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

void md_minimize_window(struct Window *window) {
	if (window == NULL || window->closed || window->minimized) return;
	// A fullscreen window that gets minimized is reconciled by window_render_layout
	// (the `fullscreen && !minimized` guard falls through to exit_fullscreen), so
	// we only set the flag here and let the layout pass issue the protocol exit.
	window->minimized = true;
	// Marca ESTA janela (a que sai p/ a barra) com o intent per-window MINIMIZE.
	window->pending_anim = ANIMATION_INTENT_MINIMIZE;
	move_last(window);          // group at the tail; newest minimized is last
	wm.target_index = 0;        // target the new MAIN (clamped below if needed)
	wm.maximized = false;
	clamp_target();
}

void md_minimize_target(void) {
	struct Window *target = target_window();
	if (target == NULL) return;
	md_minimize_window(target);
	LOG_EVENT("minimize (keybinding): \"%s\"", target->title ? target->title : "");
	log_state();
}

void md_unminimize(void) {
	// LIFO: the most recently minimized window. minimize sends each one to the
	// tail, so the LAST window with minimized==true is the newest. Filter by the
	// flag — the tail also holds non-minimized deck-overflow windows, so "last
	// in the list" alone would restore the wrong window.
	struct Window *newest = NULL;
	struct Window *w;
	wl_list_for_each(w, &wm.windows, link) {
		if (w->minimized && !w->closed) newest = w; // keep the last match
	}
	if (newest == NULL) return; // nothing minimized
	newest->minimized = false;
	// Marca ESTA janela (a que volta) com o intent per-window UNMINIMIZE. Assim o
	// relayout sabe que ela (e só ela) deve animar como unminimize, não as
	// colaterais (o DECK antigo que vira hidden deve animar como SLIDE_DECK_OUT,
	// não herdar o pending_anim global).
	newest->pending_anim = ANIMATION_INTENT_UNMINIMIZE;
	move_first(newest);         // comes back as MAIN (mirror of the prototype)
	wm.target_index = 0;
	wm.maximized = false;
	LOG_EVENT("unminimize (LIFO): \"%s\"", newest->title ? newest->title : "");
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

static void child_proposed_dimensions(struct Window *window, int32_t *width, int32_t *height, const struct LayoutView *view) {
	if (!window->implicit_parent) {
		*width = 0;
		*height = 0;
		return;
	}

	struct Window *root = root_window(window);
	struct Box pbox = output_box();
	if (root != NULL) {
		if (root == view->main_win) {
			pbox = view->main_box;
		} else if (root == view->deck_win) {
			pbox = view->deck_box;
		} else if (view->single && root == view->target) {
			pbox = view->output;
		}
	}

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
	// Visíveis ANTES da inserção decide o intent de abertura da janela nova:
	// nenhuma visível → abre solo (FADE_OPEN, scale+fade); já há visível → abre
	// em grupo (SLIDE_IN, desliza da esquerda virando o novo MAIN). Usa o count
	// de VISÍVEIS (não window_count, que inclui minimizadas na barra) p/ não
	// tratar como "grupo" um open que na tela é solo (só há uma minimizada).
	size_t visible_before = visible_window_count();
	LOG_EVENT("new window: count_before=%zu visible_before=%zu", count, visible_before);
	// DECLARATIVO: a AÇÃO de abrir marca a janela nova com seu intent. Só ela
	// anima no open; a antiga MAIN→DECK só reflui e a antiga DECK→hidden some sem
	// animação (o caminho !visible não inventa intent sem declaração per-window).
	window->pending_anim = (visible_before == 0)
		? ANIMATION_INTENT_FADE_OPEN
		: ANIMATION_INTENT_SLIDE_IN;
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

void window_manage_layout(struct Window *window, size_t index, const struct LayoutView *view) {
	// Transient windows / dialogs: compositor/client owns position/dimensions.
	if (window->parent != NULL) {
		river_window_v1_use_ssd(window->obj);
		river_window_v1_set_tiled(window->obj, RIVER_WINDOW_V1_EDGES_NONE);
		if (!window->transient_size_proposed) {
			int32_t width, height;
			child_proposed_dimensions(window, &width, &height, view);
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

	if (window->floating) {
		river_window_v1_use_ssd(window->obj);
		river_window_v1_set_tiled(window->obj, RIVER_WINDOW_V1_EDGES_NONE);
		// Sem propose_dimensions o river nunca envia o configure inicial e a
		// janela nunca é exibida ("The window will not be displayed until the
		// first dimensions event is received").
		if (!window->floating_size_proposed) {
			// (0,0): o cliente decide o próprio tamanho (protocolo permite explicitamente).
			river_window_v1_propose_dimensions(window->obj, 0, 0);
			window->floating_size_proposed = true;
			LOG_EVENT("floating natural dimensions proposed: app_id=%s",
				window->app_id ? window->app_id : "");
		}
		if (!window->floating_clamped && window->width > 0 && window->height > 0) {
			int32_t cap_w = (view->output.width  * 9) / 10;
			int32_t cap_h = (view->output.height * 9) / 10;
			if (window->width > cap_w || window->height > cap_h) {
				river_window_v1_propose_dimensions(window->obj,
					window->width > cap_w ? cap_w : window->width,
					window->height > cap_h ? cap_h : window->height);
				window->floating_clamped = true;
				LOG_EVENT("floating clamped: app_id=%s %dx%d -> cap %dx%d",
					window->app_id ? window->app_id : "",
					window->width, window->height, cap_w, cap_h);
			}
		}
		window->new = false;
		return;
	}

	// Minimized: force-hidden in render; nothing to size/tile here. Checked AFTER
	// the fullscreen exit-reconcile so minimize-from-fullscreen still issues exit.
	if (window->minimized) { window->new = false; return; }

	bool visible = view->maximized ? (window == view->target) : (index < 2 && index < (size_t)view->visible_count);
	if (!visible) return;
	struct Box box = layout_box_for_index(view, index);
	int32_t width = box.width - (BORDER_WIDTH * 2);
	// Recupera o espaço do topo removendo BORDER_WIDTH apenas uma vez (para o fundo)
	int32_t height = box.height - BORDER_WIDTH;
	river_window_v1_use_ssd(window->obj);
	river_window_v1_set_tiled(window->obj, all_edges());
	river_window_v1_propose_dimensions(window->obj, width > 1 ? width : 1, height > 1 ? height : 1);
	// Histórico das 2 últimas propostas; rotaciona só quando a proposta muda.
	int32_t nw = width > 1 ? width : 1, nh = height > 1 ? height : 1;
	if (nw != window->tile_proposed_w || nh != window->tile_proposed_h) {
		window->tile_proposed_prev_w = window->tile_proposed_w;
		window->tile_proposed_prev_h = window->tile_proposed_h;
		window->tile_proposed_w = nw;
		window->tile_proposed_h = nh;
	}
	window->new = false;
}

void window_render_layout(struct Window *window, size_t index, const struct LayoutView *view) {
	if (window->minimized) {
		bool was_visible = window->last_applied_visible;
		LOG_EVENT("[MIN-DIAG] minimized path win=\"%s\" was_visible=%d applied_visible=%d win_pending=%d",
			window->title ? window->title : "", was_visible, window->applied_visible, window->pending_anim);
		if (was_visible) {
			// DECLARATIVO PURO: a ação md_minimize_window já marcou ESTA janela com
			// MINIMIZE em window->pending_anim. Sem global, sem helper inferencial —
			// se não há intent per-window, não anima (mas o hide abaixo ainda sai).
			if (window->pending_anim != ANIMATION_INTENT_NONE) {
				md_send_animation_intent(window, (AnimationIntent)window->pending_anim);
				window->pending_anim = ANIMATION_INTENT_NONE; // one-shot
			}
			// FORÇA o hide: o guard `applied_visible == visible` em window_set_visible
			// pode pular o envio quando applied_visible já é false (divergência de
			// tracking entre applied_visible e last_applied_visible que deixa a
			// janela visível na tela com applied_visible=0). No minimize é crítico
			// que o hide CHEGUE ao river no mesmo render que o intent, senão o
			// orphan (que exige requested.hidden) nunca dispara. Enviamos direto.
			LOG_EVENT("[VIS-DIAG] hide FORCED win=\"%s\"", window->title ? window->title : "");
			river_window_v1_hide(window->obj);
			window->applied_visible = false;
		}
		window_apply_borders(window, BORDER_NONE);
		window->last_applied_visible = false;
		return;
	}
	// Transient windows / dialogs: float centered above parent, no WM borders.
	if (window->parent != NULL) {
		bool visible = window_is_really_visible_view(window, view);
		bool was_visible = window->last_applied_visible;
		if (was_visible != visible) {
			AnimationIntent intent;
			if (visible) {
				intent = md_intent_for_open(view->visible_count);
			} else {
				intent = md_intent_for_close(index, view->visible_count + 1);
			}
			md_send_animation_intent(window, intent);
		}
		window_set_visible(window, visible);
		if (visible) {
			// Center the child over the parent's layout box.
			struct Window *root = root_window(window);
			struct Box pbox;
			bool found = false;
			if (root == view->main_win) {
				pbox = view->main_box;
				found = true;
			} else if (root == view->deck_win) {
				pbox = view->deck_box;
				found = true;
			} else if (view->single && root == view->target) {
				pbox = view->output;
				found = true;
			}
			if (found) {
				int32_t cw = window->width  > 0 ? window->width  : pbox.width  / 2;
				int32_t ch = window->height > 0 ? window->height : pbox.height / 2;
				int32_t cx = pbox.x + (pbox.width  - cw) / 2;
				int32_t cy = pbox.y + (pbox.height - ch) / 2;
				if (was_visible && visible) {
					AnimationIntent intent = ANIMATION_INTENT_NONE;
					bool pos_changed = (cx != window->last_render_x || cy != window->last_render_y);
					bool size_changed = (cw != window->last_render_width || ch != window->last_render_height);
					if (size_changed) {
						intent = md_intent_for_reflow();
					} else if (pos_changed) {
						intent = md_intent_for_nudge();
					}
					if (intent != ANIMATION_INTENT_NONE) {
						md_send_animation_intent(window, intent);
					}
				}
				river_node_v1_set_position(window->node, cx, cy);
				window->last_render_x = cx;
				window->last_render_y = cy;
				window->last_render_width = cw;
				window->last_render_height = ch;
			}
			wm_place_top(window->node);
		}
		window_apply_borders(window, BORDER_NONE);
		window->last_applied_visible = visible;
		return;
	}
	// Fullscreen window: compositor owns geometry; just show it, no borders,
	// node on top (so it's the top fullscreen window — anything above it in the
	// render order, like waybar, keeps drawing; see place_top handling).
	if (window->fullscreen) {
		bool was_visible = window->last_applied_visible;
		if (!was_visible) {
			AnimationIntent intent = md_intent_for_open(view->visible_count);
			md_send_animation_intent(window, intent);
		}
		window_set_visible(window, true);
		window_apply_borders(window, BORDER_NONE);
		wm_place_top(window->node);
		window->last_applied_visible = true;
		return;
	}
	if (window->floating) {
		bool visible = true;
		bool was_visible = window->last_applied_visible;
		if (!was_visible) {
			AnimationIntent intent = md_intent_for_open(view->visible_count);
			md_send_animation_intent(window, intent);
		}
		window_set_visible(window, true);
		window_apply_borders(window, BORDER_NONE);
		// Centraliza na área útil assim que as dimensões reais são conhecidas.
		if (window->width > 0 && window->height > 0) {
			int32_t cx = view->output.x + (view->output.width - window->width) / 2;
			int32_t cy = view->output.y + (view->output.height - window->height) / 2;
			if (cx < view->output.x) cx = view->output.x;
			if (cy < view->output.y) cy = view->output.y;
			if (was_visible && visible) {
				AnimationIntent intent = ANIMATION_INTENT_NONE;
				bool pos_changed = (cx != window->last_render_x || cy != window->last_render_y);
				bool size_changed = (window->width != window->last_render_width || window->height != window->last_render_height);
				if (size_changed) {
					intent = md_intent_for_reflow();
				} else if (pos_changed) {
					intent = md_intent_for_nudge();
				}
				if (intent != ANIMATION_INTENT_NONE) {
					md_send_animation_intent(window, intent);
				}
			}
			river_node_v1_set_position(window->node, cx, cy);
			window->last_render_x = cx;
			window->last_render_y = cy;
			window->last_render_width = window->width;
			window->last_render_height = window->height;
		}
		wm_place_top(window->node);
		window->last_applied_visible = true;
		return;
	}

	bool visible = view->maximized ? (window == view->target) : (index < 2 && index < (size_t)view->visible_count);
	bool was_visible = window->last_applied_visible;
	if (was_visible != visible) {
		// DECLARATIVO PURO — SÓ per-window, ZERO fallback. O intent de uma janela
		// que cruza a fronteira de visibilidade (vira visível ou some) vem
		// EXCLUSIVAMENTE de window->pending_anim, gravado pela AÇÃO que a moveu:
		//   open→FADE_OPEN/SLIDE_IN · deck-in→DECK_IN_LEFT/RIGHT ·
		//   deck-out→SLIDE_DECK_OUT/_LEFT · minimize→MINIMIZE · unminimize→UNMINIMIZE.
		// Sem declaração → NONE (não anima): a janela empurrada p/ hidden ao abrir
		// uma 3ª janela some sem animação (igual ao protótipo), sem o SLIDE_CLOSE
		// espúrio que o helper md_intent_for_close inventava. NÃO há fallback global
		// nem inferencial aqui — se uma ação nova precisar animar uma transição de
		// visibilidade, ela DEVE marcar a janela per-window (a regra do projeto).
		AnimationIntent intent = (AnimationIntent)window->pending_anim;
		if (intent != ANIMATION_INTENT_NONE) {
			md_send_animation_intent(window, intent);
		}
		// Consome o intent per-window (one-shot) — não vaza para o próximo render.
		window->pending_anim = ANIMATION_INTENT_NONE;
	}

	if (!visible) {
		window_set_visible(window, false);
		window_apply_borders(window, BORDER_NONE);
		window->last_applied_visible = false;
		return;
	}

	// A janela está VISÍVEL neste render. Se ela estava hidden antes (vinha de
	// minimize/unminimize/deck-switch-in), envia `show` ao compositor — sem isso
	// o river mantém requested.hidden=true e a janela fica invisível (tela preta,
	// não volta do minimize). O guard interno de window_set_visible pula se já
	// aplicado, então é seguro chamar sempre aqui.
	if (was_visible != visible) {
		window_set_visible(window, true);
	}

	struct Box box = layout_box_for_index(view, index);
	int32_t new_x = box.x + BORDER_WIDTH;
	int32_t new_y = box.y;
	int32_t new_w = box.width - (BORDER_WIDTH * 2);
	int32_t new_h = box.height - BORDER_WIDTH;

	if (was_visible && visible) {
		// A janela CONTINUA visível mas mudou de geometria (swap troca slots,
		// maximize/restore cresce/encolhe, promote/send refluem). O intent vem
		// EXCLUSIVAMENTE de window->pending_anim, marcado pela AÇÃO (SPRING p/ swap,
		// REFLOW_EASE p/ maximize/restore/promote/send). NÃO há global nem
		// inferência: o `geom_changed` abaixo é só um GATE p/ não emitir quando
		// nada mexeu (re-layout por foco/manage_dirty) — o TIPO da animação é o
		// enum declarado, não o delta. Sem intent per-window → não anima
		// (reposicionamento instantâneo), evitando animar colaterais não-visadas.
		bool geom_changed = (new_x != window->last_render_x || new_y != window->last_render_y ||
		                     new_w != window->last_render_width || new_h != window->last_render_height);
		if (geom_changed && window->pending_anim != ANIMATION_INTENT_NONE) {
			md_send_animation_intent(window, (AnimationIntent)window->pending_anim);
		}
		// Consome o intent per-window (one-shot).
		window->pending_anim = ANIMATION_INTENT_NONE;
	}

	river_node_v1_set_position(window->node, new_x, new_y);
	window->last_render_x = new_x;
	window->last_render_y = new_y;
	window->last_render_width = new_w;
	window->last_render_height = new_h;
	window->last_applied_visible = true;

	// Pré-registra a animação de close DESTA janela conforme seu papel atual, para
	// que um close iniciado pelo cliente (Alt+F4 no app) tenha a direção certa sem
	// o compositor inferir por geometria. Espelha md_intent_for_close, mas declarado
	// antes do fato: solo → fade; deck (slot 1) → slide right; main-com-deck → slide left.
	{
		AnimationIntent close_intent = (view->visible_count <= 1)
			? ANIMATION_INTENT_FADE_CLOSE
			: (index == 1 ? ANIMATION_INTENT_SLIDE_DECK_OUT : ANIMATION_INTENT_SLIDE_CLOSE);
		md_send_close_intent(window, close_intent);
	}

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
	if (w == NULL) return false;
	struct LayoutView view;
	layout_view_init(&view);
	return window_is_really_visible_view(w, &view);
}

bool window_is_really_visible_view(struct Window *w, const struct LayoutView *view) {
	int depth = 0;
	while (w != NULL) {
		if (w->minimized) return false;
		if (w->parent == NULL) break;
		if (++depth > 32) return false;   // guarda anti-ciclo / aninhamento patológico
		w = w->parent;
	}
	if (w == NULL) return false;
	if (view->single) {
		return w == view->target;
	}
	return w == view->main_win || w == view->deck_win;
}
