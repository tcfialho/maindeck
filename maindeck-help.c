// maindeck-help — painel de atalhos do MainDeck (Super+F1).
//
// Uma layer-surface OVERLAY em tela cheia: fundo escurecido + painel central com
// a lista de atalhos. Qualquer tecla ou clique fecha. Instância única: rodar de
// novo enquanto aberto fecha a instância existente (toggle).
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cairo.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define PANEL_WIDTH 560
#define PANEL_PADDING 28
#define ROW_HEIGHT 30
#define KEY_COLUMN_WIDTH 230
#define TITLE_HEIGHT 48

struct Shortcut {
	const char *keys;
	const char *description;
};

static const struct Shortcut SHORTCUTS[] = {
	{ "Super+Tab", "alternar foco: MAIN → DECK → destacada" },
	{ "Super+Tab  2×", "próxima carta do DECK" },
	{ "Super+Tab  segurar", "trocar MAIN ↔ DECK" },
	{ "Super+→  /  ←", "carta seguinte / anterior" },
	{ "Super+→  segurar", "mandar alvo para o fundo do DECK" },
	{ "Super+←  segurar", "promover alvo a MAIN" },
	{ "Super+↑", "maximizar  (segurar: trazer minimizada)" },
	{ "Super+↓", "restaurar  (segurar: minimizar)" },
	{ "Super+D", "destacar / encaixar janela" },
	{ "Super+arrastar", "mover janela destacada" },
	{ "Super+botão direito", "redimensionar janela destacada" },
	{ "Super+Enter", "abrir terminal" },
	{ "Super+Del  /  Alt+F4", "fechar janela" },
	{ "Win", "menu iniciar" },
	{ "Print", "captura de tela" },
	{ "Super+F1", "esta ajuda" },
};
#define SHORTCUT_COUNT (sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]))

struct HelpState {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wl_buffer *buffer;
	int width, height;
	bool running;
};

static struct HelpState g_help = { .running = true };

static void close_help(void) { g_help.running = false; }

static int create_shm_file(size_t size) {
	int fd = memfd_create("maindeck-help", MFD_CLOEXEC);
	if (fd < 0) return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -1.5708, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 1.5708);
	cairo_arc(cr, x + r, y + h - r, r, 1.5708, 3.1416);
	cairo_arc(cr, x + r, y + r, r, 3.1416, 4.7124);
	cairo_close_path(cr);
}

static void draw_text(cairo_t *cr, PangoLayout *layout, const char *text, double x, double y,
                      double r, double g, double b) {
	pango_layout_set_text(layout, text, -1);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
}

static void draw_panel(cairo_t *cr, int width, int height) {
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	int panel_h = PANEL_PADDING * 2 + TITLE_HEIGHT + (int)SHORTCUT_COUNT * ROW_HEIGHT + 24;
	double px = (width - PANEL_WIDTH) / 2.0;
	double py = (height - panel_h) / 2.0;

	rounded_rect(cr, px, py, PANEL_WIDTH, panel_h, 10);
	cairo_set_source_rgb(cr, 0.11, 0.11, 0.14);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.22, 0.22, 0.30);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *title_font = pango_font_description_from_string("sans bold 14");
	pango_layout_set_font_description(layout, title_font);
	draw_text(cr, layout, "Atalhos MainDeck", px + PANEL_PADDING, py + PANEL_PADDING, 0.92, 0.92, 0.92);
	pango_font_description_free(title_font);

	PangoFontDescription *key_font = pango_font_description_from_string("sans bold 10");
	PangoFontDescription *desc_font = pango_font_description_from_string("sans 10");
	double row_y = py + PANEL_PADDING + TITLE_HEIGHT;
	for (size_t i = 0; i < SHORTCUT_COUNT; i++) {
		double key_x = px + PANEL_PADDING;
		pango_layout_set_font_description(layout, key_font);
		pango_layout_set_text(layout, SHORTCUTS[i].keys, -1);
		int key_w, key_h;
		pango_layout_get_pixel_size(layout, &key_w, &key_h);
		rounded_rect(cr, key_x - 6, row_y + 1, key_w + 12, ROW_HEIGHT - 6, 4);
		cairo_set_source_rgb(cr, 0.18, 0.36, 0.70);
		cairo_fill(cr);
		draw_text(cr, layout, SHORTCUTS[i].keys, key_x, row_y + (ROW_HEIGHT - 6 - key_h) / 2.0 + 1, 1, 1, 1);

		pango_layout_set_font_description(layout, desc_font);
		draw_text(cr, layout, SHORTCUTS[i].description, key_x + KEY_COLUMN_WIDTH,
		          row_y + (ROW_HEIGHT - 6 - key_h) / 2.0 + 1, 0.85, 0.85, 0.88);
		row_y += ROW_HEIGHT;
	}

	PangoFontDescription *hint_font = pango_font_description_from_string("sans 9");
	pango_layout_set_font_description(layout, hint_font);
	draw_text(cr, layout, "Qualquer tecla ou clique fecha", px + PANEL_PADDING, row_y + 6, 0.55, 0.55, 0.60);
	pango_font_description_free(hint_font);
	pango_font_description_free(key_font);
	pango_font_description_free(desc_font);
	g_object_unref(layout);
}

static void render(void) {
	int stride = g_help.width * 4;
	size_t size = (size_t)stride * (size_t)g_help.height;
	int fd = create_shm_file(size);
	if (fd < 0) { close_help(); return; }
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) { close(fd); close_help(); return; }

	cairo_surface_t *cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32,
		g_help.width, g_help.height, stride);
	cairo_t *cr = cairo_create(cs);
	draw_panel(cr, g_help.width, g_help.height);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	struct wl_shm_pool *pool = wl_shm_create_pool(g_help.shm, fd, (int32_t)size);
	if (g_help.buffer) wl_buffer_destroy(g_help.buffer);
	g_help.buffer = wl_shm_pool_create_buffer(pool, 0, g_help.width, g_help.height, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	munmap(data, size);
	close(fd);

	wl_surface_attach(g_help.surface, g_help.buffer, 0, 0);
	wl_surface_damage_buffer(g_help.surface, 0, 0, g_help.width, g_help.height);
	wl_surface_commit(g_help.surface);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial,
                            uint32_t width, uint32_t height) {
	(void)data;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	g_help.width = width > 0 ? (int)width : 1920;
	g_help.height = height > 0 ? (int)height : 1080;
	render();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
	(void)data; (void)surface;
	close_help();
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int fd, uint32_t s) {
	(void)d; (void)k; (void)f; (void)s;
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *a) {
	(void)d; (void)k; (void)s; (void)sf; (void)a;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
	(void)d; (void)k; (void)s; (void)sf;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t state) {
	(void)d; (void)k; (void)s; (void)t; (void)key;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) close_help();
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
	(void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g;
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t de) {
	(void)d; (void)k; (void)r; (void)de;
}
static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
	.key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)s; (void)sf; (void)x; (void)y;
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {
	(void)d; (void)p; (void)s; (void)sf;
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)t; (void)x; (void)y;
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t state) {
	(void)d; (void)p; (void)s; (void)t; (void)b;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) close_help();
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
	(void)d; (void)p; (void)t; (void)a; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
	.axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps) {
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_help.keyboard) {
		g_help.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(g_help.keyboard, &keyboard_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_help.pointer) {
		g_help.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(g_help.pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities, .name = seat_name,
};

static void registry_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
	(void)d;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		g_help.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		g_help.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && !g_help.seat) {
		g_help.seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(g_help.seat, &seat_listener, NULL);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		g_help.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	}
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

// Instância única via lock; se já existe, fecha a existente (toggle) e sai.
static bool acquire_single_instance(void) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir) return true;
	char path[256];
	snprintf(path, sizeof(path), "%s/maindeck-help.pid", dir);
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) return true;
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		char buf[32] = {0};
		if (read(fd, buf, sizeof(buf) - 1) > 0) {
			pid_t pid = (pid_t)atoi(buf);
			if (pid > 0) kill(pid, SIGTERM);
		}
		close(fd);
		return false;
	}
	if (ftruncate(fd, 0) == 0) dprintf(fd, "%d\n", (int)getpid());
	return true; // fd fica aberto (e travado) até o processo sair
}

static void handle_sigterm(int sig) { (void)sig; g_help.running = false; }

int main(void) {
	if (!acquire_single_instance()) return 0;
	signal(SIGTERM, handle_sigterm);

	g_help.display = wl_display_connect(NULL);
	if (!g_help.display) {
		fprintf(stderr, "maindeck-help: sem conexão Wayland\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(g_help.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(g_help.display);
	if (!g_help.compositor || !g_help.shm || !g_help.layer_shell) {
		fprintf(stderr, "maindeck-help: compositor sem layer-shell/shm\n");
		return 1;
	}

	g_help.surface = wl_compositor_create_surface(g_help.compositor);
	g_help.layer_surface = zwlr_layer_shell_v1_get_layer_surface(g_help.layer_shell, g_help.surface,
		NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "maindeck-help");
	zwlr_layer_surface_v1_set_anchor(g_help.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(g_help.layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(g_help.layer_surface, 1); // exclusive
	zwlr_layer_surface_v1_add_listener(g_help.layer_surface, &layer_listener, NULL);
	wl_surface_commit(g_help.surface);

	while (g_help.running && wl_display_dispatch(g_help.display) != -1) {
	}

	if (g_help.buffer) wl_buffer_destroy(g_help.buffer);
	zwlr_layer_surface_v1_destroy(g_help.layer_surface);
	wl_surface_destroy(g_help.surface);
	wl_display_flush(g_help.display);
	wl_display_disconnect(g_help.display);
	return 0;
}
