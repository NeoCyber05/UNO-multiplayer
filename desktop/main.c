#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include "tcp_client.h"
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Client *g_client = NULL;
static WebKitWebView *g_web_view = NULL;
static GIOChannel *g_io_channel = NULL;
static guint g_io_watch_id = 0;

static gboolean on_socket_io(GIOChannel *source, GIOCondition condition, gpointer data);

static void update_socket_watch(void)
{
    if (g_io_watch_id > 0) {
        g_source_remove(g_io_watch_id);
        g_io_watch_id = 0;
    }
    if (!g_io_channel || !g_client) return;

    GIOCondition cond = G_IO_IN | G_IO_HUP | G_IO_ERR;
    if (g_client->conn.out_len > g_client->conn.out_pos) {
        cond |= G_IO_OUT;
    }
    g_io_watch_id = g_io_add_watch(g_io_channel, cond, on_socket_io, g_web_view);
}

static void on_client_pending_write(void *ctx, bool has_pending)
{
    (void)ctx;
    (void)has_pending;
    update_socket_watch();
}

static Status on_client_frame(const Envelope *e, void *ctx)
{
    WebKitWebView *web_view = (WebKitWebView *)ctx;
    bridge_emit(web_view, e);
    return OK;
}

static gboolean on_socket_io(GIOChannel *source, GIOCondition condition, gpointer data)
{
    (void)source;
    WebKitWebView *web_view = (WebKitWebView *)data;

    if (condition & G_IO_IN) {
        Status s = client_read(g_client, on_client_frame, web_view);
        if (s != OK) {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }
    }

    if (condition & G_IO_OUT) {
        Status s = conn_flush(&g_client->conn);
        if (s != OK) {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }
        if (g_client->conn.out_len == g_client->conn.out_pos) {
            update_socket_watch();
            return G_SOURCE_REMOVE;
        }
    }

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean on_decide_policy(WebKitWebView *web_view,
                                 WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type,
                                 gpointer user_data)
{
    (void)web_view;
    (void)user_data;

    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ||
        type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationPolicyDecision *nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
        WebKitURIRequest *req = webkit_navigation_action_get_request(action);
        const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;

        if (!uri || (strncmp(uri, "uno://app/", 10) != 0 && strcmp(uri, "uno://app") != 0)) {
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
    }

    return FALSE;
}

static void on_spike_done(void *ctx)
{
    (void)ctx;
    gtk_main_quit();
}

static gboolean on_timeout(gpointer data)
{
    (void)data;
    fprintf(stderr, "desktop: safety timeout reached\n");
    gtk_main_quit();
    exit(1);
    return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    uint16_t port = 9090;
    const char *assets = "./client/dist";
    const char *uri = "uno://app/index.html";
    int timeout_sec = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_sec = atoi(argv[++i]);
        }
    }

    bridge_set_assets_dir(assets);
    bridge_set_done_callback(on_spike_done, NULL);

    Status st = client_connect(&g_client, host, port);
    if (st != OK || !g_client) {
        fprintf(stderr, "desktop: failed to connect to %s:%u (status=%d)\n", host, port, st);
        return 1;
    }

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1024, 768);
    gtk_window_set_title(GTK_WINDOW(window), "UNO Multiplayer");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *view_widget = webkit_web_view_new();
    g_web_view = WEBKIT_WEB_VIEW(view_widget);
    g_signal_connect(g_web_view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    gtk_container_add(GTK_CONTAINER(window), view_widget);

    st = bridge_init(g_web_view, g_client);
    if (st != OK) {
        fprintf(stderr, "desktop: failed to initialize bridge\n");
        client_close(g_client);
        return 1;
    }

    int fd = client_fd(g_client);
    g_io_channel = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(g_io_channel, NULL, NULL);
    g_io_channel_set_buffered(g_io_channel, FALSE);
    client_set_pending_callback(g_client, on_client_pending_write, NULL);
    update_socket_watch();

    if (timeout_sec > 0) {
        g_timeout_add_seconds(timeout_sec, on_timeout, NULL);
    }

    webkit_web_view_load_uri(g_web_view, uri);
    gtk_widget_show_all(window);

    gtk_main();

    if (g_io_watch_id > 0) {
        g_source_remove(g_io_watch_id);
        g_io_watch_id = 0;
    }
    if (g_io_channel) {
        g_io_channel_unref(g_io_channel);
        g_io_channel = NULL;
    }

    client_close(g_client);
    g_client = NULL;
    return 0;
}
