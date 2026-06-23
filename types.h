#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"
#include "river-layer-shell-v1-client-protocol.h"
#include "river-libinput-config-v1-client-protocol.h"
#include "river-input-management-v1-client-protocol.h"

struct wp_cursor_shape_device_v1;

struct Output {
	struct river_output_v1 *obj;
	struct river_layer_shell_output_v1 *shell_output;
	bool removed;
	bool default_set;
	bool presentation_mode_set;
	bool scanout_capture_logged;
	uint32_t presentation_mode;
	uint32_t capture_session_count;
	int32_t x, y, width, height;
	// Área utilizável após subtrair as exclusive zones das layer shells (waybar).
	// Zero até o primeiro non_exclusive_area chegar — fallback para dimensões do output.
	int32_t usable_x, usable_y, usable_width, usable_height;
	struct wl_list link;
};

enum BorderState {
	BORDER_UNINITIALIZED = 0,
	BORDER_NONE,
	BORDER_TILED
};

struct Window {
	struct river_window_v1 *obj;
	struct river_node_v1 *node;
	struct Window *parent;

	bool new;
	bool closed;
	bool transient_size_proposed;
	bool implicit_parent;
	bool floating;
	bool floating_size_proposed;
	int32_t width, height;
	int32_t min_width, min_height, max_width, max_height;
	char *app_id;
	char *title;
	char *identifier; /* river_window.identifier — único por janela, usado para taskbar activate */
	bool autofloat;              // virou floating por heurística (não rouba foco)
	bool tile_settled;           // já obedeceu uma proposta de tile (≈ tolerância); nunca mais reclassifica por mismatch
	bool floating_clamped;       // clamp de flutuante grande já foi feito (1x só)
	// Duas últimas propostas de tile (0 = nenhuma). Duas porque uma janela lenta
	// pode responder à proposta anterior depois de um relayout já ter registrado
	// uma nova — essa resposta atrasada não é desobediência.
	int32_t tile_proposed_w, tile_proposed_h;
	int32_t tile_proposed_prev_w, tile_proposed_prev_h;

	// Client-requested fullscreen (e.g. a game). Separate from wm.maximized
	// (Super+Up), which only fills the usable area. Fullscreen covers the whole
	// output, including waybar.
	bool fullscreen;
	bool minimized; // force-hidden independente do índice; excluído da contagem visível; agrupado na cauda de wm.windows
	bool applied_fullscreen; // what we last told the server, to act only on edges
	struct river_output_v1 *fs_output;
	uint32_t presentation_hint;
	uint32_t capture_session_count;

	enum BorderState border_state;
	bool border_focused;
	bool applied_visible;

	bool last_applied_visible;
	int32_t last_render_x, last_render_y;
	int32_t last_render_width, last_render_height;
	// Último close_intent pré-registrado no compositor (AnimationIntent). -1 =
	// nunca enviado. Evita reenviar set_close_intent a cada relayout se não mudou.
	int32_t last_close_intent;
	// Animação declarada pela AÇÃO para ESTA janela (one-shot, per-window). A ação
	// marca a janela visada (ex.: UNMINIMIZE na que volta, MINIMIZE na que sai) e o
	// render cycle lê/consome. Diferente do pending_anim global do WindowManager,
	// este é específico — não contamina janelas colaterais que mudam de slot por
	// efeito da mesma ação. 0 (NONE) = sem intent per-window declarado.
	uint32_t pending_anim;

	struct wl_list link; // WindowManager.windows in MainDeck order
};

enum Action {
	ACTION_NONE,
	ACTION_SPAWN_TERMINAL,
	ACTION_SPAWN_LAUNCHER,
	ACTION_SPAWN_SCREENSHOT,
	ACTION_CLOSE_TARGET,
	ACTION_TOGGLE_TARGET,
	ACTION_SWAP_MAIN_DECK,
	ACTION_DECK_NEXT,
	ACTION_DECK_PREV,
	ACTION_SEND_TARGET_TO_DECK_BOTTOM,
	ACTION_PROMOTE_TARGET_TO_MAIN,
	ACTION_MAXIMIZE_TARGET,
	ACTION_RESTORE,
	ACTION_TOGGLE_MAXIMIZE,
	ACTION_MINIMIZE_TARGET,
	ACTION_UNMINIMIZE,
	ACTION_EXIT,
};

/* Ação pedida pela barra (menu de contexto) sobre uma janela identificada por
 * identifier. Agendada pelo IPC e aplicada dentro do manage cycle, espelhando
 * pending_activate_identifier. WINDOW_ACTION_NONE = nada pendente. */
enum WindowAction {
	WINDOW_ACTION_NONE = 0,
	WINDOW_ACTION_MINIMIZE,
	WINDOW_ACTION_MAXIMIZE,
	WINDOW_ACTION_RESTORE,
};

typedef enum WindowAction WindowAction;

struct XkbBinding {
	struct river_xkb_binding_v1 *obj;
	struct Seat *seat;
	enum Action tap_action;
	enum Action hold_action; // ACTION_NONE if no hold behavior
	bool held;
	bool hold_fired;
	struct timespec press_time;
	struct wl_list link;
};

struct PointerBinding {
	struct river_pointer_binding_v1 *obj;
	struct Seat *seat;
	enum Action action;
	struct wl_list link;
};

struct Seat {
	struct river_seat_v1 *obj;
	struct river_layer_shell_seat_v1 *layer_shell_seat;
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;
	struct wp_cursor_shape_device_v1 *cursor_shape_device;
	uint32_t wl_seat_name;
	bool new;
	bool removed;

	struct Window *focused;
	struct Window *interacted;

	struct wl_list xkb_bindings;
	struct wl_list pointer_bindings;
	enum Action pending_action;

	struct wl_list link;
};

struct WindowManager {
	struct wl_list outputs;
	struct wl_list windows; // MainDeck order: 0=MAIN, 1=visible DECK, 2+=hidden
	struct wl_list seats;
	uint32_t target_index; // 0 or 1
	bool maximized;
	bool focus_dirty;
	// Animação declarada pela AÇÃO (one-shot). A ação que muda o layout grava aqui
	// QUAL animação a transição deve ter (ANIMATION_INTENT_*); o render cycle a
	// transmite às janelas afetadas e a zera no fim. ANIMATION_INTENT_NONE = sem
	// animação declarada (o render não inventa uma por geometria). Substitui a
	// antiga inferência por diff de pixels (size_changed/pos_changed<50).
	uint32_t pending_anim;
	struct river_node_v1 *last_placed_top_node;
	// Flutuante a focar no próximo manage_start (focus_window é window
	// management state e só pode ser feito dentro da manage sequence).
	struct Window *pending_float_focus;
};

struct DeviceState {
	struct river_libinput_device_v1 *libinput_dev;
	struct river_input_device_v1 *input_dev;
	char *name;
	uint32_t type;
	bool is_touchpad;
	bool has_tap_support;
};

struct Box {
	int32_t x, y, width, height;
};

#endif /* TYPES_H */
