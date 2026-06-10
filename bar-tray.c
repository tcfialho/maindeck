#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>

#include <stdint.h>
#include <dbus/dbus.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

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
static char              g_menu_service[128] = "";

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

struct PropsRequest {
    char service[128];
};

static void on_props_reply(DBusPendingCall *pending, void *user_data) {
    struct PropsRequest *req = user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);

    if (!reply) {
        dbus_pending_call_unref(pending);
        return;
    }

    /* Lookup the item by stable service name */
    struct TrayItem *it = NULL;
    for (int i = 0; i < g_item_n; i++) {
        if (strcmp(g_items[i].service, req->service) == 0) {
            it = &g_items[i];
            break;
        }
    }

    if (!it) {
        /* Item was removed before the reply arrived, discard response */
        dbus_message_unref(reply);
        dbus_pending_call_unref(pending);
        return;
    }

    DBusMessageIter iter, arr;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        dbus_pending_call_unref(pending);
        return;
    }
    dbus_message_iter_recurse(&iter, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&arr, &entry);

        const char *key = NULL;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);

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
    dbus_pending_call_unref(pending);

    /* Load icon surface (TrayItem owns its reference simetrically) */
    if (it->icon_name[0]) {
        cairo_surface_t *new_icon = bar_icon_get(it->icon_name, ICON_SIZE);
        if (!new_icon) new_icon = bar_icon_get(it->service, ICON_SIZE);
        if (new_icon) cairo_surface_reference(new_icon);
        if (it->icon) cairo_surface_destroy(it->icon);
        it->icon = new_icon;
    }

    LOG_INFO("tray: item %s icon='%s' title='%s'",
             it->service, it->icon_name, it->title);
    bar_request_redraw(&g_bar);
}

static void free_props_request(void *user_data) {
    free(user_data);
}

static void load_item_props(struct TrayItem *it) {
    DBusMessage *msg = dbus_message_new_method_call(
        it->service, it->obj_path,
        "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) return;

    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &(const char *){ SNI_ITEM_IFACE },
        DBUS_TYPE_INVALID);

    DBusPendingCall *pending = NULL;
    if (dbus_connection_send_with_reply(g_conn, msg, &pending, 1000) && pending) {
        struct PropsRequest *req = malloc(sizeof(struct PropsRequest));
        bool ok = false;
        if (req) {
            snprintf(req->service, sizeof(req->service), "%s", it->service);
            if (dbus_pending_call_set_notify(pending, on_props_reply, req, free_props_request)) {
                ok = true;
            } else {
                free(req);
            }
        }
        if (!ok) {
            dbus_pending_call_cancel(pending);
            dbus_pending_call_unref(pending);
        }
    }
    dbus_message_unref(msg);
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
    bar_request_redraw(&g_bar);
}

static void remove_item(const char *reg_str) {
    char svc[128], path[128];
    parse_sni_service(reg_str, svc, sizeof(svc), path, sizeof(path));

    for (int i = 0; i < g_item_n; i++) {
        if (strcmp(g_items[i].service, svc) == 0) {
            if (g_items[i].icon) {
                cairo_surface_destroy(g_items[i].icon);
                g_items[i].icon = NULL;
            }
            /* shift */
            int rem = g_item_n - i - 1;
            if (rem > 0)
                memmove(&g_items[i], &g_items[i+1],
                        (size_t)rem * sizeof(g_items[0]));
            g_item_n--;
            bar_request_redraw(&g_bar);
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

    int msg_type = dbus_message_get_type(msg);
    const char *iface  = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    const char *path   = dbus_message_get_path(msg);
    if (!iface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (msg_type == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        if (path && strcmp(path, SNI_WATCHER_PATH) == 0) {
            if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
                if (strcmp(member, "Get") == 0) {
                    const char *req_iface = NULL;
                    const char *req_prop = NULL;
                    dbus_message_get_args(msg, NULL,
                                          DBUS_TYPE_STRING, &req_iface,
                                          DBUS_TYPE_STRING, &req_prop,
                                          DBUS_TYPE_INVALID);
                    if (req_iface && strcmp(req_iface, SNI_WATCHER_IFACE) == 0) {
                        if (req_prop && strcmp(req_prop, "RegisteredStatusNotifierItems") == 0) {
                            DBusMessage *reply = dbus_message_new_method_return(msg);
                            if (reply) {
                                DBusMessageIter iter, variant, arr;
                                dbus_message_iter_init_append(reply, &iter);
                                dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "as", &variant);
                                dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
                                for (int i = 0; i < g_item_n; i++) {
                                    char full_path[256];
                                    snprintf(full_path, sizeof(full_path), "%s%s", 
                                             g_items[i].service, g_items[i].obj_path);
                                    const char *s = full_path;
                                    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &s);
                                }
                                dbus_message_iter_close_container(&variant, &arr);
                                dbus_message_iter_close_container(&iter, &variant);
                                dbus_connection_send(conn, reply, NULL);
                                dbus_message_unref(reply);
                                return DBUS_HANDLER_RESULT_HANDLED;
                            }
                        } else if (req_prop && strcmp(req_prop, "IsStatusNotifierHostRegistered") == 0) {
                            DBusMessage *reply = dbus_message_new_method_return(msg);
                            if (reply) {
                                DBusMessageIter iter, variant;
                                dbus_message_iter_init_append(reply, &iter);
                                dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
                                dbus_bool_t val = TRUE;
                                dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
                                dbus_message_iter_close_container(&iter, &variant);
                                dbus_connection_send(conn, reply, NULL);
                                dbus_message_unref(reply);
                                return DBUS_HANDLER_RESULT_HANDLED;
                            }
                        } else if (req_prop && strcmp(req_prop, "ProtocolVersion") == 0) {
                            DBusMessage *reply = dbus_message_new_method_return(msg);
                            if (reply) {
                                DBusMessageIter iter, variant;
                                dbus_message_iter_init_append(reply, &iter);
                                dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "i", &variant);
                                dbus_int32_t val = 0;
                                dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &val);
                                dbus_message_iter_close_container(&iter, &variant);
                                dbus_connection_send(conn, reply, NULL);
                                dbus_message_unref(reply);
                                return DBUS_HANDLER_RESULT_HANDLED;
                            }
                        }
                    }
                } else if (strcmp(member, "GetAll") == 0) {
                    const char *req_iface = NULL;
                    dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &req_iface, DBUS_TYPE_INVALID);
                    if (req_iface && strcmp(req_iface, SNI_WATCHER_IFACE) == 0) {
                        DBusMessage *reply = dbus_message_new_method_return(msg);
                        if (reply) {
                            DBusMessageIter iter, dict;
                            dbus_message_iter_init_append(reply, &iter);
                            dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
                            
                            {
                                DBusMessageIter entry, variant, arr;
                                const char *key = "RegisteredStatusNotifierItems";
                                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
                                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
                                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
                                dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
                                for (int i = 0; i < g_item_n; i++) {
                                    char full_path[256];
                                    snprintf(full_path, sizeof(full_path), "%s%s", 
                                             g_items[i].service, g_items[i].obj_path);
                                    const char *s = full_path;
                                    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &s);
                                }
                                dbus_message_iter_close_container(&variant, &arr);
                                dbus_message_iter_close_container(&entry, &variant);
                                dbus_message_iter_close_container(&dict, &entry);
                            }
                            
                            {
                                DBusMessageIter entry, variant;
                                const char *key = "IsStatusNotifierHostRegistered";
                                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
                                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
                                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
                                dbus_bool_t val = TRUE;
                                dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
                                dbus_message_iter_close_container(&entry, &variant);
                                dbus_message_iter_close_container(&dict, &entry);
                            }
                            
                            {
                                DBusMessageIter entry, variant;
                                const char *key = "ProtocolVersion";
                                dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
                                dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
                                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &variant);
                                dbus_int32_t val = 0;
                                dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &val);
                                dbus_message_iter_close_container(&entry, &variant);
                                dbus_message_iter_close_container(&dict, &entry);
                            }
                            
                            dbus_message_iter_close_container(&iter, &dict);
                            dbus_connection_send(conn, reply, NULL);
                            dbus_message_unref(reply);
                            return DBUS_HANDLER_RESULT_HANDLED;
                        }
                    }
                }
            } else if (strcmp(iface, SNI_WATCHER_IFACE) == 0) {
                if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
                    const char *arg = NULL;
                    dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
                    if (arg) {
                        char full_name[256];
                        const char *sender = dbus_message_get_sender(msg);
                        if (arg[0] == '/') {
                            snprintf(full_name, sizeof(full_name), "%s%s", sender ? sender : "", arg);
                        } else if (strchr(arg, '/') == NULL) {
                            snprintf(full_name, sizeof(full_name), "%s", arg);
                        } else {
                            snprintf(full_name, sizeof(full_name), "%s", arg);
                        }
                        LOG_INFO("watcher: RegisterStatusNotifierItem '%s' (sender=%s)", full_name, sender ? sender : "");
                        add_item(full_name);

                        DBusMessage *sig = dbus_message_new_signal(
                            SNI_WATCHER_PATH, SNI_WATCHER_IFACE, "StatusNotifierItemRegistered");
                        if (sig) {
                            const char *sig_arg = full_name;
                            dbus_message_append_args(sig, DBUS_TYPE_STRING, &sig_arg, DBUS_TYPE_INVALID);
                            dbus_connection_send(conn, sig, NULL);
                            dbus_message_unref(sig);
                        }
                    }
                    
                    DBusMessage *reply = dbus_message_new_method_return(msg);
                    if (reply) {
                        dbus_connection_send(conn, reply, NULL);
                        dbus_message_unref(reply);
                    }
                    return DBUS_HANDLER_RESULT_HANDLED;
                } else if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
                    DBusMessage *reply = dbus_message_new_method_return(msg);
                    if (reply) {
                        dbus_connection_send(conn, reply, NULL);
                        dbus_message_unref(reply);
                    }
                    DBusMessage *sig = dbus_message_new_signal(
                        SNI_WATCHER_PATH, SNI_WATCHER_IFACE, "StatusNotifierHostRegistered");
                    if (sig) {
                        dbus_connection_send(conn, sig, NULL);
                        dbus_message_unref(sig);
                    }
                    return DBUS_HANDLER_RESULT_HANDLED;
                }
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (msg_type == DBUS_MESSAGE_TYPE_SIGNAL) {
        if (strcmp(iface, "org.freedesktop.DBus") == 0 && strcmp(member, "NameOwnerChanged") == 0) {
            const char *name = NULL;
            const char *old_owner = NULL;
            const char *new_owner = NULL;
            dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner,
                                  DBUS_TYPE_INVALID);
            if (name && new_owner && strcmp(new_owner, "") == 0) {
                remove_item(name);
                if (old_owner && strcmp(old_owner, name) != 0) {
                    remove_item(old_owner);
                }
            }
        } else if (strcmp(iface, SNI_WATCHER_IFACE) == 0) {
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

    /* Request StatusNotifierWatcher ownership to become the watcher */
    bool is_watcher = false;
    int ret_watcher = dbus_bus_request_name(g_conn, SNI_WATCHER_BUS,
                                            DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
    if (ret_watcher == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        ret_watcher == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        is_watcher = true;
        LOG_INFO("tray: registered as primary StatusNotifierWatcher");
    } else {
        if (dbus_error_is_set(&err)) {
            LOG_WARN("tray: request watcher name failed: %s", err.message);
            dbus_error_free(&err);
        } else {
            LOG_INFO("tray: watcher already active (fallback to host-only mode)");
        }
    }

    /* Register as StatusNotifierHost */
    snprintf(g_host_name, sizeof(g_host_name),
             "org.kde.StatusNotifierHost-%d", (int)getpid());

    int ret = dbus_bus_request_name(g_conn, g_host_name,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret < 0 || dbus_error_is_set(&err)) {
        LOG_WARN("tray: request_name failed: %s",
                 dbus_error_is_set(&err) ? err.message : "unknown");
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
    }

    if (!is_watcher) {
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
    }

    /* Add match rules for watcher signals and method calls */
    dbus_bus_add_match(g_conn, "type='signal',interface='" SNI_WATCHER_IFACE "'", &err);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);

    if (is_watcher) {
        dbus_bus_add_match(g_conn, "type='method_call',interface='" SNI_WATCHER_IFACE "'", &err);
        if (dbus_error_is_set(&err)) dbus_error_free(&err);

        dbus_bus_add_match(g_conn, "type='method_call',interface='org.freedesktop.DBus.Properties'", &err);
        if (dbus_error_is_set(&err)) dbus_error_free(&err);

        dbus_bus_add_match(g_conn, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'", &err);
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
    }

    dbus_connection_add_filter(g_conn, signal_filter, NULL, NULL);
    dbus_connection_flush(g_conn);

    if (!is_watcher) {
        /* Enumerate items already registered on external watcher */
        enumerate_items();
    }

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
    dbus_connection_read_write(g_conn, 0);
    while (dbus_connection_dispatch(g_conn) == DBUS_DISPATCH_DATA_REMAINS) {
        /* drena toda a fila de mensagens já bufferizada */
    }
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

static void tray_send_method(struct TrayItem *it, const char *method, int x, int y) {
    DBusMessage *msg = dbus_message_new_method_call(
        it->service, it->obj_path, SNI_ITEM_IFACE, method);
    if (!msg) return;

    dbus_int32_t ix = (dbus_int32_t)x;
    dbus_int32_t iy = (dbus_int32_t)y;
    dbus_message_append_args(msg,
        DBUS_TYPE_INT32, &ix,
        DBUS_TYPE_INT32, &iy,
        DBUS_TYPE_INVALID);

    dbus_connection_send(g_conn, msg, NULL);
    dbus_connection_flush(g_conn);
    dbus_message_unref(msg);
    LOG_INFO("tray: %s(%d,%d) → %s %s", method, x, y, it->service, it->obj_path);
}

void bar_tray_click(int idx, int button, int x, int y) {
    if (!g_conn || idx < 0 || idx >= g_item_n) return;
    struct TrayItem *it = &g_items[idx];

    if (button == 1) {
        tray_send_method(it, "Activate", x, y);
    } else {
        /* Right click: open dbusmenu popup if available, else ContextMenu */
        bar_tray_open_menu(idx, x, 0, g_bar.last_btn_serial);
    }
}

/* ------------------------------------------------------------------ */
/* dbusmenu popup                                                       */
/* ------------------------------------------------------------------ */

#define MENU_ITEM_MAX  16
#define MENU_PAD_X     14
#define MENU_PAD_Y     6   /* vertical padding above/below text in each row */
#define MENU_MIN_W     160
#define MENU_SEP_H     9

struct MenuItem {
    int   id;
    char  label[128];
    bool  separator;
    bool  enabled;
};

static struct MenuItem  g_menu_items[MENU_ITEM_MAX];
static int              g_menu_item_n    = 0;
static int              g_menu_hover     = -1;   /* hovered row */
static int              g_menu_anchor_x  = 0;
static int              g_menu_anchor_w  = 0;
static uint32_t         g_menu_serial    = 0;
static int              g_menu_tray_idx  = -1;

static void menu_shm_create(int w, int h) {
    struct BarState *bar = &g_bar;
    int stride = w * 4;
    int size   = stride * h;

    char name[32];
    snprintf(name, sizeof(name), "/maindeck-menu-%d", (int)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_TRUNC, 0600);
    shm_unlink(name);
    if (fd < 0) { LOG_WARN("menu: shm_open failed"); return; }
    if (ftruncate(fd, size) < 0) { close(fd); return; }

    void *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return; }

    struct wl_shm_pool *pool = wl_shm_create_pool(bar->shm, fd, size);
    close(fd);

    bar->menu_pool   = pool;
    bar->menu_buf    = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                           WL_SHM_FORMAT_ARGB8888);
    bar->menu_data   = data;
    bar->menu_fd     = -1;
    bar->menu_width  = w;
    bar->menu_height = h;
    bar->menu_stride = stride;
}

/* Measure all items with Pango to get accurate row heights and max width.
 * Fills g_menu_items[i].row_h (stored temporarily in label[127] trick — no,
 * just use a parallel array). We store row heights in a static array. */
static int g_menu_row_h[MENU_ITEM_MAX];  /* pixel height of each row */
static int g_menu_text_h = 16;           /* single-line text height, measured once */

static void menu_measure(void) {
    /* Create a throw-away cairo/pango context to measure text */
    cairo_surface_t *dummy = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(dummy);
    PangoLayout *lay = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    pango_layout_set_single_paragraph_mode(lay, TRUE);

    /* measure one line height */
    pango_layout_set_text(lay, "Ag", -1);
    int tw, th;
    pango_layout_get_pixel_size(lay, &tw, &th);
    g_menu_text_h = th;

    for (int i = 0; i < g_menu_item_n; i++) {
        if (g_menu_items[i].separator) {
            g_menu_row_h[i] = MENU_SEP_H;
        } else {
            g_menu_row_h[i] = g_menu_text_h + MENU_PAD_Y * 2;
        }
    }

    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_destroy(dummy);
}

static int menu_calc_width(void) {
    cairo_surface_t *dummy = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(dummy);
    PangoLayout *lay = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    pango_layout_set_single_paragraph_mode(lay, TRUE);

    int max_w = MENU_MIN_W;
    for (int i = 0; i < g_menu_item_n; i++) {
        if (g_menu_items[i].separator) continue;
        pango_layout_set_text(lay, g_menu_items[i].label, -1);
        int tw, th; (void)th;
        pango_layout_get_pixel_size(lay, &tw, &th);
        int w = tw + MENU_PAD_X * 2;
        if (w > max_w) max_w = w;
    }

    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_destroy(dummy);
    return max_w;
}

static int menu_calc_height(void) {
    int h = 8; /* 4px top + 4px bottom padding */
    for (int i = 0; i < g_menu_item_n; i++)
        h += g_menu_row_h[i];
    return h;
}

static void menu_render(void) {
    struct BarState *bar = &g_bar;
    if (!bar->menu_data) return;

    int w = bar->menu_width;
    int h = bar->menu_height;

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        (unsigned char *)bar->menu_data,
        CAIRO_FORMAT_ARGB32, w, h, bar->menu_stride);
    cairo_t *cr = cairo_create(cs);

    /* Background */
    cairo_set_source_rgba(cr, 0.16, 0.16, 0.20, 0.97);
    cairo_paint(cr);

    /* Border */
    cairo_set_source_rgba(cr, 0.40, 0.40, 0.50, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_stroke(cr);

    PangoLayout *lay = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);
    pango_layout_set_single_paragraph_mode(lay, TRUE);

    int y = 4;
    for (int i = 0; i < g_menu_item_n; i++) {
        struct MenuItem *item = &g_menu_items[i];
        int rh = g_menu_row_h[i];

        if (item->separator) {
            cairo_set_source_rgba(cr, 0.40, 0.40, 0.50, 0.8);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, MENU_PAD_X, y + rh / 2.0);
            cairo_line_to(cr, w - MENU_PAD_X, y + rh / 2.0);
            cairo_stroke(cr);
            y += rh;
            continue;
        }

        bool hovered = (i == g_menu_hover) && item->enabled;
        if (hovered) {
            cairo_set_source_rgba(cr, 0.25, 0.40, 0.72, 1.0);
            cairo_rectangle(cr, 2, y, w - 4, rh);
            cairo_fill(cr);
        }

        if (item->enabled)
            cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, 1.0);
        else
            cairo_set_source_rgba(cr, 0.50, 0.50, 0.55, 1.0);

        pango_layout_set_text(lay, item->label, -1);
        cairo_move_to(cr, MENU_PAD_X, y + MENU_PAD_Y);
        pango_cairo_show_layout(cr, lay);

        y += rh;
    }

    g_object_unref(lay);
    cairo_destroy(cr);
    cairo_surface_finish(cs);
    cairo_surface_destroy(cs);

    wl_surface_attach(bar->menu_surface, bar->menu_buf, 0, 0);
    wl_surface_damage_buffer(bar->menu_surface, 0, 0, w, h);
    wl_surface_commit(bar->menu_surface);
}

/* Parse props a{sv} iter into a MenuItem */
static void parse_item_props(DBusMessageIter *props_arr, struct MenuItem *mi) {
    while (dbus_message_iter_get_arg_type(props_arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, val;
        dbus_message_iter_recurse(props_arr, &entry);

        const char *key = NULL;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);

        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, &val);
            int vt = dbus_message_iter_get_arg_type(&val);
            if (key && strcmp(key, "label") == 0 && vt == DBUS_TYPE_STRING) {
                const char *lbl = NULL;
                dbus_message_iter_get_basic(&val, &lbl);
                if (lbl) snprintf(mi->label, sizeof(mi->label), "%s", lbl);
            } else if (key && strcmp(key, "type") == 0 && vt == DBUS_TYPE_STRING) {
                const char *t = NULL;
                dbus_message_iter_get_basic(&val, &t);
                if (t && strcmp(t, "separator") == 0) mi->separator = true;
            } else if (key && strcmp(key, "enabled") == 0 && vt == DBUS_TYPE_BOOLEAN) {
                dbus_bool_t en = TRUE;
                dbus_message_iter_get_basic(&val, &en);
                mi->enabled = (bool)en;
            }
        }
        dbus_message_iter_next(props_arr);
    }
}

/* Parse one (ia{sv}av) struct from an iter into g_menu_items */
static void parse_node(DBusMessageIter *node) {
    /* id */
    if (dbus_message_iter_get_arg_type(node) != DBUS_TYPE_INT32) return;
    dbus_int32_t item_id = 0;
    dbus_message_iter_get_basic(node, &item_id);
    dbus_message_iter_next(node);

    /* props a{sv} */
    if (dbus_message_iter_get_arg_type(node) != DBUS_TYPE_ARRAY) return;

    struct MenuItem mi;
    memset(&mi, 0, sizeof(mi));
    mi.id      = (int)item_id;
    mi.enabled = true;

    DBusMessageIter props;
    dbus_message_iter_recurse(node, &props);
    parse_item_props(&props, &mi);
    dbus_message_iter_next(node);

    /* Only add leaf items (not the root node id=0, no label) */
    if (item_id != 0 && g_menu_item_n < MENU_ITEM_MAX) {
        g_menu_items[g_menu_item_n++] = mi;
    }
}

/* Parse dbusmenu GetLayout reply: out u revision, out (ia{sv}av) layout */
static void parse_menu_layout(DBusMessage *reply) {
    g_menu_item_n = 0;

    DBusMessageIter top;
    dbus_message_iter_init(reply, &top);

    /* skip revision (uint32) */
    if (dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_UINT32) return;
    dbus_message_iter_next(&top);

    /* root node: (ia{sv}av) */
    if (dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_STRUCT) return;
    DBusMessageIter root;
    dbus_message_iter_recurse(&top, &root);

    /* parse root node itself (id=0, skip it) then read children av */
    /* id */
    if (dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_INT32) return;
    dbus_message_iter_next(&root); /* skip root id */
    /* props */
    if (dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_next(&root); /* skip root props */
    /* children: av */
    if (dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) return;

    DBusMessageIter children;
    dbus_message_iter_recurse(&root, &children);

    while (dbus_message_iter_get_arg_type(&children) == DBUS_TYPE_VARIANT) {
        DBusMessageIter child_var;
        dbus_message_iter_recurse(&children, &child_var);

        if (dbus_message_iter_get_arg_type(&child_var) == DBUS_TYPE_STRUCT) {
            DBusMessageIter child_node;
            dbus_message_iter_recurse(&child_var, &child_node);
            parse_node(&child_node);
        }
        dbus_message_iter_next(&children);
    }

    LOG_INFO("menu: parsed %d items", g_menu_item_n);
}

static void menu_popup_done(void *data, struct xdg_popup *popup) {
    (void)data; (void)popup;
    LOG_INFO("menu: popup_done");
    bar_tray_menu_close();
}

static void menu_popup_configure(void *data, struct xdg_popup *popup,
    int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)data; (void)popup; (void)x; (void)y; (void)w; (void)h;
}

static void menu_popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token) {
    (void)data; (void)popup; (void)token;
}

static const struct xdg_popup_listener menu_popup_listener = {
    .configure     = menu_popup_configure,
    .popup_done    = menu_popup_done,
    .repositioned  = menu_popup_repositioned,
};

static void menu_xdg_surface_configure(void *data, struct xdg_surface *xdg_surf,
    uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(xdg_surf, serial);
    menu_render();
}

static const struct xdg_surface_listener menu_xdg_surface_listener = {
    .configure = menu_xdg_surface_configure,
};

struct MenuLayoutRequest {
    char service[128];
    int icon_x;
    int icon_w;
    uint32_t serial;
};

static void on_menu_layout_reply(DBusPendingCall *pending, void *user_data) {
    struct MenuLayoutRequest *req = user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);

    if (!reply) {
        dbus_pending_call_unref(pending);
        return;
    }

    /* Lookup the item by stable service name */
    int idx = -1;
    for (int i = 0; i < g_item_n; i++) {
        if (strcmp(g_items[i].service, req->service) == 0) {
            idx = i;
            break;
        }
    }

    /* If the item was removed or another menu request was started, discard */
    if (idx == -1 || strcmp(req->service, g_menu_service) != 0) {
        dbus_message_unref(reply);
        dbus_pending_call_unref(pending);
        return;
    }
    g_menu_tray_idx = idx; // update index in case of item shift

    parse_menu_layout(reply);
    dbus_message_unref(reply);
    dbus_pending_call_unref(pending);

    if (g_menu_item_n == 0) {
        LOG_WARN("menu: no items from GetLayout");
        return;
    }

    menu_measure();
    int mw = menu_calc_width();
    int mh = menu_calc_height();

    struct BarState *bar = &g_bar;

    /* Create wl_surface for popup */
    bar->menu_surface = wl_compositor_create_surface(bar->compositor);
    bar->menu_xdg_surface = xdg_wm_base_get_xdg_surface(bar->xdg_wm_base, bar->menu_surface);
    xdg_surface_add_listener(bar->menu_xdg_surface, &menu_xdg_surface_listener, NULL);

    /* Positioner: anchor at icon position on the bar, grow upward */
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(bar->xdg_wm_base);
    xdg_positioner_set_size(pos, mw, mh);
    xdg_positioner_set_anchor_rect(pos, req->icon_x, 0, g_menu_anchor_w, bar->height);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP_LEFT);
    xdg_positioner_set_constraint_adjustment(pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);

    bar->menu_popup = xdg_surface_get_popup(bar->menu_xdg_surface, NULL, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(bar->menu_popup, &menu_popup_listener, NULL);

    /* Attach popup to the layer surface */
    zwlr_layer_surface_v1_get_popup(bar->layer_surface, bar->menu_popup);

    /* Grab input focus */
    xdg_popup_grab(bar->menu_popup, bar->seat, req->serial);

    /* Allocate SHM buffer */
    menu_shm_create(mw, mh);
    bar->menu_open = true;

    /* Commit so compositor sends configure */
    wl_surface_commit(bar->menu_surface);
    wl_display_flush(bar->display);

    LOG_INFO("menu: opened for %s (%d items) %dx%d", req->service, g_menu_item_n, mw, mh);
    bar_request_redraw(bar);
}

void bar_tray_open_menu(int idx, int icon_x, int icon_w, uint32_t serial) {
    if (!g_conn || idx < 0 || idx >= g_item_n) return;
    struct BarState *bar = &g_bar;
    if (!bar->xdg_wm_base) {
        LOG_WARN("menu: no xdg_wm_base");
        return;
    }

    bar_tray_menu_close();

    struct TrayItem *it = &g_items[idx];
    g_menu_tray_idx  = idx;
    g_menu_anchor_x  = icon_x;
    g_menu_anchor_w  = icon_w > 0 ? icon_w : 22;
    g_menu_serial    = serial;
    g_menu_hover     = -1;
    g_menu_item_n    = 0;
    snprintf(g_menu_service, sizeof(g_menu_service), "%s", it->service);

    /* Build menu path: SNI obj_path + "/Menu" OR read the Menu property */
    char menu_path[256];
    snprintf(menu_path, sizeof(menu_path), "%s/Menu", it->obj_path);

    /* GetLayout(parentId=0, recursionDepth=1, propertyNames=[]) */
    DBusMessage *msg = dbus_message_new_method_call(
        it->service, menu_path, "com.canonical.dbusmenu", "GetLayout");
    if (!msg) return;

    dbus_int32_t parent_id = 0, depth = 1;
    DBusMessageIter args, arr;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &parent_id);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &depth);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &arr);
    dbus_message_iter_close_container(&args, &arr);

    DBusPendingCall *pending = NULL;
    if (dbus_connection_send_with_reply(g_conn, msg, &pending, 2000) && pending) {
        struct MenuLayoutRequest *req = malloc(sizeof(struct MenuLayoutRequest));
        bool ok = false;
        if (req) {
            snprintf(req->service, sizeof(req->service), "%s", it->service);
            req->icon_x = icon_x;
            req->icon_w = icon_w;
            req->serial = serial;
            if (dbus_pending_call_set_notify(pending, on_menu_layout_reply, req, free_props_request)) {
                ok = true;
            } else {
                free(req);
            }
        }
        if (!ok) {
            dbus_pending_call_cancel(pending);
            dbus_pending_call_unref(pending);
        }
    }
    dbus_message_unref(msg);
}

int bar_tray_menu_row_at(int y) {
    int cy = 4;
    for (int i = 0; i < g_menu_item_n; i++) {
        int rh = g_menu_row_h[i];
        if (!g_menu_items[i].separator && y >= cy && y < cy + rh)
            return i;
        cy += rh;
    }
    return -1;
}

void bar_tray_menu_activate(int row, uint32_t time) {
    if (row < 0 || row >= g_menu_item_n) return;
    struct MenuItem *mi = &g_menu_items[row];
    if (mi->separator || !mi->enabled) return;

    if (g_menu_tray_idx < 0 || g_menu_tray_idx >= g_item_n) return;
    struct TrayItem *it = &g_items[g_menu_tray_idx];

    char menu_path[256];
    snprintf(menu_path, sizeof(menu_path), "%s/Menu", it->obj_path);

    DBusMessage *msg = dbus_message_new_method_call(
        it->service, menu_path, "com.canonical.dbusmenu", "Event");
    if (!msg) return;

    dbus_int32_t item_id = (dbus_int32_t)mi->id;
    const char *event_id = "clicked";
    dbus_uint32_t timestamp = (dbus_uint32_t)time;

    DBusMessageIter args;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &item_id);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &event_id);
    /* data: variant with empty string */
    DBusMessageIter var;
    dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &var);
    const char *empty = "";
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &empty);
    dbus_message_iter_close_container(&args, &var);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &timestamp);

    dbus_connection_send(g_conn, msg, NULL);
    dbus_connection_flush(g_conn);
    dbus_message_unref(msg);

    LOG_INFO("menu: Event(%d, clicked) → %s", mi->id, mi->label);
    bar_tray_menu_close();
}

void bar_tray_menu_rerend(void) {
    struct BarState *bar = &g_bar;
    if (!bar->menu_open || !bar->menu_data) return;
    g_menu_hover = bar->menu_hover_row;
    menu_render();
}

void bar_tray_menu_close(void) {
    struct BarState *bar = &g_bar;
    if (!bar->menu_open) return;

    if (bar->menu_popup) {
        xdg_popup_destroy(bar->menu_popup);
        bar->menu_popup = NULL;
    }
    if (bar->menu_xdg_surface) {
        xdg_surface_destroy(bar->menu_xdg_surface);
        bar->menu_xdg_surface = NULL;
    }
    if (bar->menu_buf) {
        wl_buffer_destroy(bar->menu_buf);
        bar->menu_buf = NULL;
    }
    if (bar->menu_pool) {
        wl_shm_pool_destroy(bar->menu_pool);
        bar->menu_pool = NULL;
    }
    if (bar->menu_data) {
        int size = bar->menu_stride * bar->menu_height;
        munmap(bar->menu_data, (size_t)size);
        bar->menu_data = NULL;
    }
    if (bar->menu_surface) {
        wl_surface_destroy(bar->menu_surface);
        bar->menu_surface = NULL;
    }

    bar->menu_open      = false;
    bar->menu_tray_idx  = -1;
    g_menu_tray_idx     = -1;
    g_menu_item_n       = 0;
    LOG_INFO("menu: closed");
}

/* ------------------------------------------------------------------ */
/* Public: cleanup                                                      */
/* ------------------------------------------------------------------ */

void bar_tray_cleanup(void) {
    if (!g_conn) return;
    dbus_connection_remove_filter(g_conn, signal_filter, NULL);
    dbus_connection_unref(g_conn);
    g_conn = NULL;
    for (int i = 0; i < g_item_n; i++) {
        if (g_items[i].icon) {
            cairo_surface_destroy(g_items[i].icon);
            g_items[i].icon = NULL;
        }
    }
    g_item_n = 0;
}
