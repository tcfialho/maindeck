#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "bar-tray.h"
#include "bar-state.h"
#include "bar-icons.h"
#include "bar-render.h"
#include "bar-log.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define SNI_WATCHER_BUS  "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_PATH "/StatusNotifierWatcher"
#define SNI_WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define SNI_ITEM_IFACE   "org.kde.StatusNotifierItem"
#define TRAY_MAX         16
#define ICON_SIZE        18

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

struct TrayItem {
    char   service[128];   /* e.g. ":1.27" */
    char   obj_path[128];  /* e.g. "/org/ayatana/NotificationItem/trayid48171" */
    char   icon_name[128];
    char   title[64];
    cairo_surface_t *icon;
    bool   active;
};

static DBusConnection   *g_conn     = NULL;
static struct TrayItem   g_items[TRAY_MAX];
static int               g_item_n   = 0;
static char              g_host_name[64];

/* ------------------------------------------------------------------ */
/* Parse ":1.27/obj/path" → service + obj_path                         */
/* ------------------------------------------------------------------ */

static void parse_sni_service(const char *reg, char *svc, size_t svc_cap,
                               char *path, size_t path_cap) {
    const char *slash = strchr(reg, '/');
    if (slash) {
        size_t slen = (size_t)(slash - reg);
        if (slen >= svc_cap) slen = svc_cap - 1;
        memcpy(svc, reg, slen);
        svc[slen] = '\0';
        snprintf(path, path_cap, "%s", slash);
    } else {
        snprintf(svc,  svc_cap,  "%s", reg);
        snprintf(path, path_cap, "/StatusNotifierItem");
    }
}

/* ------------------------------------------------------------------ */
/* Read properties from an SNI item                                     */
/* ------------------------------------------------------------------ */

static void load_item_props(struct TrayItem *it) {
    DBusMessage *msg = dbus_message_new_method_call(
        it->service, it->obj_path,
        "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) return;

    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &(const char *){ SNI_ITEM_IFACE },
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        g_conn, msg, 1000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        return;
    }

    DBusMessageIter iter, arr;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return;
    }
    dbus_message_iter_recurse(&iter, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&arr, &entry);

        const char *key = NULL;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);

        /* entry now points to the value (variant) */
        int vtype = dbus_message_iter_get_arg_type(&entry);

        if (vtype == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, &variant);
            int inner = dbus_message_iter_get_arg_type(&variant);
            if (inner == DBUS_TYPE_STRING) {
                const char *val = NULL;
                dbus_message_iter_get_basic(&variant, &val);
                if (val) {
                    if (strcmp(key, "IconName") == 0)
                        snprintf(it->icon_name, sizeof(it->icon_name), "%s", val);
                    else if (strcmp(key, "Title") == 0)
                        snprintf(it->title, sizeof(it->title), "%s", val);
                }
            }
        }
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(reply);

    /* Load icon */
    if (it->icon_name[0]) {
        it->icon = bar_icon_get(it->icon_name, ICON_SIZE);
        if (!it->icon) {
            /* many SNI icons use reverse-DNS app_id as icon name — try as-is */
            LOG_WARN("tray: no icon for '%s' (item: %s)", it->icon_name, it->service);
        }
    }
    LOG_INFO("tray: item %s icon='%s' title='%s'",
             it->service, it->icon_name, it->title);
}

/* ------------------------------------------------------------------ */
/* Add / remove items                                                   */
/* ------------------------------------------------------------------ */

static void add_item(const char *reg_str) {
    if (g_item_n >= TRAY_MAX) return;

    char svc[128], path[128];
    parse_sni_service(reg_str, svc, sizeof(svc), path, sizeof(path));

    /* dedup */
    for (int i = 0; i < g_item_n; i++)
        if (strcmp(g_items[i].service, svc) == 0) return;

    struct TrayItem *it = &g_items[g_item_n];
    memset(it, 0, sizeof(*it));
    snprintf(it->service,  sizeof(it->service),  "%s", svc);
    snprintf(it->obj_path, sizeof(it->obj_path), "%s", path);
    it->active = true;

    load_item_props(it);
    g_item_n++;
    g_bar.dirty = true;
}

static void remove_item(const char *reg_str) {
    char svc[128], path[128];
    parse_sni_service(reg_str, svc, sizeof(svc), path, sizeof(path));

    for (int i = 0; i < g_item_n; i++) {
        if (strcmp(g_items[i].service, svc) == 0) {
            /* shift */
            int rem = g_item_n - i - 1;
            if (rem > 0)
                memmove(&g_items[i], &g_items[i+1],
                        (size_t)rem * sizeof(g_items[0]));
            g_item_n--;
            g_bar.dirty = true;
            LOG_INFO("tray: removed item %s", svc);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Signal filter                                                        */
/* ------------------------------------------------------------------ */

static DBusHandlerResult signal_filter(DBusConnection *conn,
    DBusMessage *msg, void *data)
{
    (void)conn; (void)data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface  = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!iface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(iface, SNI_WATCHER_IFACE) == 0) {
        const char *arg = NULL;
        dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
        if (!arg) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        if (strcmp(member, "StatusNotifierItemRegistered") == 0) {
            LOG_INFO("tray: NewStatusNotifierItem '%s'", arg);
            add_item(arg);
        } else if (strcmp(member, "StatusNotifierItemUnregistered") == 0) {
            LOG_INFO("tray: StatusNotifierItemUnregistered '%s'", arg);
            remove_item(arg);
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Enumerate existing items at startup                                  */
/* ------------------------------------------------------------------ */

static void enumerate_items(void) {
    DBusMessage *msg = dbus_message_new_method_call(
        SNI_WATCHER_BUS, SNI_WATCHER_PATH,
        "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return;

    const char *iface = SNI_WATCHER_IFACE;
    const char *prop  = "RegisteredStatusNotifierItems";
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        g_conn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        if (dbus_error_is_set(&err)) {
            LOG_WARN("tray: enumerate failed: %s", err.message);
            dbus_error_free(&err);
        }
        return;
    }

    DBusMessageIter iter, variant, arr;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) goto done;
    dbus_message_iter_recurse(&iter, &variant);
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) goto done;
    dbus_message_iter_recurse(&variant, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *s = NULL;
        dbus_message_iter_get_basic(&arr, &s);
        if (s) add_item(s);
        dbus_message_iter_next(&arr);
    }

done:
    dbus_message_unref(reply);
}

/* ------------------------------------------------------------------ */
/* Public: init                                                         */
/* ------------------------------------------------------------------ */

int bar_tray_init(void) {
    DBusError err;
    dbus_error_init(&err);

    g_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_conn) {
        LOG_WARN("tray: dbus_bus_get failed: %s", err.message);
        dbus_error_free(&err);
        return -1;
    }

    /* Don't exit on disconnect — let bar handle it */
    dbus_connection_set_exit_on_disconnect(g_conn, FALSE);

    /* Register as StatusNotifierHost */
    snprintf(g_host_name, sizeof(g_host_name),
             "org.kde.StatusNotifierHost-%d", (int)getpid());

    int ret = dbus_bus_request_name(g_conn, g_host_name,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret < 0 || dbus_error_is_set(&err)) {
        LOG_WARN("tray: request_name failed: %s",
                 dbus_error_is_set(&err) ? err.message : "unknown");
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        /* non-fatal: we can still watch items */
    }

    /* Tell the watcher we exist */
    DBusMessage *reg = dbus_message_new_method_call(
        SNI_WATCHER_BUS, SNI_WATCHER_PATH,
        SNI_WATCHER_IFACE, "RegisterStatusNotifierHost");
    if (reg) {
        dbus_message_append_args(reg,
            DBUS_TYPE_STRING, &(const char *){ g_host_name },
            DBUS_TYPE_INVALID);
        dbus_connection_send(g_conn, reg, NULL);
        dbus_message_unref(reg);
    }

    /* Watch watcher signals */
    dbus_bus_add_match(g_conn,
        "type='signal',interface='" SNI_WATCHER_IFACE "'", &err);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);

    dbus_connection_add_filter(g_conn, signal_filter, NULL, NULL);
    dbus_connection_flush(g_conn);

    /* Enumerate items already registered */
    enumerate_items();

    int fd = -1;
    if (!dbus_connection_get_unix_fd(g_conn, &fd)) {
        LOG_WARN("tray: cannot get DBus unix fd — tray events won't be polled");
        return -1;
    }

    LOG_INFO("tray: init OK, %d items, fd=%d", g_item_n, fd);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Public: dispatch                                                     */
/* ------------------------------------------------------------------ */

void bar_tray_dispatch(void) {
    if (!g_conn) return;
    while (dbus_connection_read_write_dispatch(g_conn, 0))
        ;
}

/* ------------------------------------------------------------------ */
/* Public: accessors                                                    */
/* ------------------------------------------------------------------ */

int bar_tray_count(void) { return g_item_n; }

cairo_surface_t *bar_tray_icon(int idx) {
    if (idx < 0 || idx >= g_item_n) return NULL;
    return g_items[idx].icon;
}

const char *bar_tray_title(int idx) {
    if (idx < 0 || idx >= g_item_n) return "";
    return g_items[idx].title;
}

/* ------------------------------------------------------------------ */
/* Public: click                                                        */
/* ------------------------------------------------------------------ */

void bar_tray_click(int idx, int button, int x, int y) {
    if (!g_conn || idx < 0 || idx >= g_item_n) return;

    const char *method = (button == 1) ? "Activate" : "ContextMenu";
    struct TrayItem *it = &g_items[idx];

    DBusMessage *msg = dbus_message_new_method_call(
        it->service, it->obj_path, SNI_ITEM_IFACE, method);
    if (!msg) return;

    dbus_int32_t ix = (dbus_int32_t)x;
    dbus_int32_t iy = (dbus_int32_t)y;
    dbus_message_append_args(msg,
        DBUS_TYPE_INT32, &ix,
        DBUS_TYPE_INT32, &iy,
        DBUS_TYPE_INVALID);

    /* fire and forget */
    dbus_connection_send(g_conn, msg, NULL);
    dbus_connection_flush(g_conn);
    dbus_message_unref(msg);
    LOG_INFO("tray: %s(%d,%d) → %s", method, x, y, it->service);
}

/* ------------------------------------------------------------------ */
/* Public: cleanup                                                      */
/* ------------------------------------------------------------------ */

void bar_tray_cleanup(void) {
    if (!g_conn) return;
    dbus_connection_remove_filter(g_conn, signal_filter, NULL);
    dbus_connection_unref(g_conn);
    g_conn = NULL;
    g_item_n = 0;
}
