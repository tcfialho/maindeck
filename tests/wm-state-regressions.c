// Exercise real WM handlers with recorded protocol requests, under ASan/UBSan.
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../wm-handlers.c"
#include "../wm-input.c"

struct WindowManager wm;
struct wl_display *wm_display;
struct wl_registry *wm_registry;
struct river_window_manager_v1 *window_manager_v1;
struct river_xkb_bindings_v1 *xkb_bindings_v1;
struct river_layer_shell_v1 *layer_shell_v1;
struct river_libinput_config_v1 *libinput_config_v1;
struct wp_cursor_shape_manager_v1 *cursor_shape_manager_v1;
char pending_activate_identifier[33];
enum WindowAction pending_window_action;
char pending_window_action_id[33];

static bool fail_allocation;
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size) {
    if (fail_allocation) { fail_allocation = false; return NULL; }
    return __real_calloc(n, size);
}

static struct river_window_v1 *focused_request;
static struct river_window_v1 *shown_request;
static int positioned, nodes_destroyed;
uint32_t wl_proxy_get_version(struct wl_proxy *proxy) { return 7; }
struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
        const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    va_list args;
    va_start(args, flags);
    if (proxy == (struct wl_proxy *)0x100 && opcode == RIVER_SEAT_V1_FOCUS_WINDOW)
        focused_request = va_arg(args, struct river_window_v1 *);
    if (proxy == (struct wl_proxy *)0x200 && opcode == RIVER_WINDOW_V1_SHOW)
        shown_request = (struct river_window_v1 *)proxy;
    if (proxy == (struct wl_proxy *)0x300 && opcode == RIVER_NODE_V1_SET_POSITION)
        positioned++;
    if (proxy == (struct wl_proxy *)0x300 && opcode == RIVER_NODE_V1_DESTROY)
        nodes_destroyed++;
    va_end(args);
    return NULL;
}
static struct Window *add_window(bool pending) {
    struct Window *w = calloc(1, sizeof(*w));
    assert(w);
    w->obj = (struct river_window_v1 *)0x200;
    w->node = (struct river_node_v1 *)0x300;
    w->pending = pending;
    w->width = 640; w->height = 480;
    wl_list_insert((pending ? &wm.pending_windows : &wm.windows)->prev, &w->link);
    return w;
}
static void add_seat(struct Seat *s) {
    memset(s, 0, sizeof(*s));
    s->obj = (struct river_seat_v1 *)0x100;
    wl_list_init(&s->xkb_bindings); wl_list_init(&s->pointer_bindings);
    wl_list_insert(&wm.seats, &s->link);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    wm_init();
    if (!strcmp(argv[1], "pending-parent-close")) {
        struct Window *parent = add_window(false), *child = add_window(true);
        child->parent = parent;
        parent->closed = true;
        window_maybe_destroy(parent);
        // Classification/logging used to dereference the freed parent here.
        classify_pending_windows();
        assert(child->parent == NULL);
    } else if (!strcmp(argv[1], "floating-child-visible")) {
        struct Window *parent = add_window(false), *child = add_window(false);
        parent->floating = true; child->parent = parent;
        struct LayoutView view; layout_view_init(&view);
        assert(window_is_really_visible_view(child, &view));
        window_render_layout(child, 0, &view);
        assert(child->last_applied_visible && shown_request == child->obj);
        assert(positioned);
    } else if (!strcmp(argv[1], "pending-float-focus")) {
        struct Window *a = add_window(false), *b = add_window(false);
        a->floating = b->floating = true;
        struct Seat seat; add_seat(&seat); seat.focused = a;
        wm.pending_float_focus = b;
        focus_target_on_seats();
        assert(seat.focused == b && focused_request == b->obj);
    } else if (!strcmp(argv[1], "taskbar-from-floating")) {
        struct Window *root = add_window(false), *floating = add_window(false);
        floating->floating = true;
        struct Seat seat; add_seat(&seat); seat.focused = floating;
        root->identifier = strdup("01234567890123456789012345678901");
        snprintf(pending_activate_identifier, sizeof(pending_activate_identifier), "%s", root->identifier);
        apply_pending_taskbar_activation();
        focus_target_on_seats();
        assert(seat.focused == root);
    } else if (!strcmp(argv[1], "taskbar-floating-child")) {
        struct Window *root = add_window(false), *a = add_window(false), *b = add_window(false);
        root->floating = true; a->parent = b->parent = root;
        struct Seat seat; add_seat(&seat); seat.focused = b;
        a->identifier = strdup("01234567890123456789012345678901");
        snprintf(pending_activate_identifier, sizeof(pending_activate_identifier), "%s", a->identifier);
        apply_pending_taskbar_activation();
        focus_target_on_seats();
        assert(seat.focused == a);
    } else if (!strcmp(argv[1], "minimize-child")) {
        struct Window *root = add_window(false), *child = add_window(false);
        child->parent = root;
        md_minimize_window(child);
        assert(!child->minimized && target_window() == root);
        md_maximize_window(child);
        assert(!wm.maximized && target_window() == root);
    } else if (!strcmp(argv[1], "orphan-after-minimized")) {
        struct Window *parent = add_window(false), *minimized = add_window(false), *child = add_window(false);
        minimized->minimized = true; child->parent = parent;
        parent->closed = true;
        window_maybe_destroy(parent);
        assert(target_window() == child);
        assert(window_index(minimized) == -1);
    } else if (!strcmp(argv[1], "pending-minimize")) {
        struct Window *w = add_window(true);
        md_minimize_window(w);
        assert(w->pending && wl_list_empty(&wm.windows));
        classify_pending_windows();
        assert(!w->pending && w->minimized);
        assert(visible_window_count() == 0 && target_window() == NULL);
    } else if (!strcmp(argv[1], "node-released")) {
        struct Window *w = add_window(false);
        w->closed = true;
        window_maybe_destroy(w);
        assert(nodes_destroyed == 1);
    } else if (!strcmp(argv[1], "output-removed-fullscreen")) {
        struct Output *o = calloc(1, sizeof(*o));
        o->obj = (struct river_output_v1 *)0x400;
        o->removed = true;
        wl_list_insert(&wm.outputs, &o->link);
        struct Window *w = add_window(false);
        w->fullscreen = true; w->applied_fullscreen = true; w->fs_output = o->obj;
        output_maybe_destroy(o);
        assert(w->fs_output == NULL && !w->applied_fullscreen);
    } else if (!strcmp(argv[1], "xkb-allocation-failure")) {
        struct Seat seat; add_seat(&seat);
        fail_allocation = true;
        xkb_binding_create(&seat, 0, XKB_KEY_Tab, ACTION_NONE, ACTION_NONE);
        assert(wl_list_empty(&seat.xkb_bindings));
    } else if (!strcmp(argv[1], "pointer-allocation-failure")) {
        struct Seat seat; add_seat(&seat);
        fail_allocation = true;
        pointer_binding_create(&seat, 0, BTN_LEFT, ACTION_NONE);
        assert(wl_list_empty(&seat.pointer_bindings));
    } else if (!strcmp(argv[1], "snapshot-deduplication")) {
        char directory[] = "/tmp/maindeck-snapshot-test.XXXXXX";
        assert(mkdtemp(directory));
        setenv("XDG_RUNTIME_DIR",directory,1);
        struct sockaddr_un address = { .sun_family = AF_UNIX };
        snprintf(address.sun_path,sizeof(address.sun_path),"%s/maindeck-bar.sock",directory);
        int receiver = socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0);
        assert(receiver>=0 && bind(receiver,(struct sockaddr *)&address,sizeof(address))==0);
        struct Window *w = add_window(false);
        w->identifier = strdup("01234567890123456789012345678901");
        for(int i=0;i<100;i++) wm_notify_bar_windows();
        char buffer[4096];
        assert(recv(receiver,buffer,sizeof(buffer),0)>0);
        assert(recv(receiver,buffer,sizeof(buffer),0)<0 && errno==EAGAIN);
        wm_request_bar_snapshot(); wm_notify_bar_windows();
        assert(recv(receiver,buffer,sizeof(buffer),0)>0);
        w->minimized=true; wm_notify_bar_windows();
        assert(recv(receiver,buffer,sizeof(buffer),0)>0);
        assert(recv(receiver,buffer,sizeof(buffer),0)<0 && errno==EAGAIN);
        close(receiver); unlink(address.sun_path); rmdir(directory);
    } else if (!strcmp(argv[1], "random-state-sequence")) {
        struct Window *roots[8];
        for (size_t i = 0; i < 8; i++) roots[i] = add_window(false);
        struct Window *dialog = add_window(false);
        dialog->parent = roots[3];
        struct Window *floating = add_window(false);
        floating->floating = true;
        srand(0x4d444543);
        for (size_t i = 0; i < 5000; i++) {
            struct Window *w = roots[rand() % 8];
            switch (rand() % 9) {
            case 0: md_minimize_window(w); break;
            case 1: md_restore_window(w); break;
            case 2: md_swap_main_deck(); break;
            case 3: if (visible_window_count() > 2) md_deck_next(); break;
            case 4: if (visible_window_count() > 2) md_deck_prev(); break;
            case 5: md_send_target_to_deck_bottom(); break;
            case 6: md_promote_target_to_main(); break;
            case 7: md_maximize_window(w); break;
            case 8: md_unminimize(); break;
            }
            clamp_target();
            struct LayoutView view; layout_view_init(&view);
            assert(window_count() == 8);
            assert(view.visible_count == (int)visible_window_count());
            assert(target_window() == (wm.target_index == 0 ? view.main_win : view.deck_win));
            struct Window *target = target_window();
            assert(target == NULL || (!target->minimized && !target->floating && target->parent == NULL));
            size_t entries = 0;
            struct Window *item;
            wl_list_for_each(item, &wm.windows, link) {
                assert(++entries <= 10);
                if (item->minimized) assert(window_index(item) == -1);
            }
            assert(entries == 10);
        }
    } else if (!strcmp(argv[1], "declared-parent-preserved")) {
        struct Window *a = add_window(false), *b = add_window(false);
        b->parent = a;
        // Opt-in adoption must preserve an existing explicit parent relation.
        a->app_id = strdup("quirk"); b->app_id = strdup("quirk");
        a->title = strdup("CHILD"); b->title = strdup("PARENT");
        setenv("MAINDECK_IMPLICIT_PARENT_APP_ID", "quirk", 1);
        setenv("MAINDECK_IMPLICIT_PARENT_TITLES", "PARENT", 1);
        maybe_apply_implicit_parenting();
        assert(root_window(b) == a);
    } else {
        fprintf(stderr, "unknown case: %s\n", argv[1]); return 2;
    }
    puts("PASS");
    return 0;
}
