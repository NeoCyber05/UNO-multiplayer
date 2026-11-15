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

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
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
    int timeout_sec = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets = argv[++i];
        } else if (strcmp(argv[i], "--uri") == 0 && i + 1 < argc) {
            uri = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_sec = atoi(argv[++i]);
        }
    }

    const char *env_uri = getenv("UNO_URI");
    if (env_uri && *env_uri) {
        uri = env_uri;
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
    gtk_container_add(GTK_CONTAINER(window), view_widget);

    st = bridge_init(g_web_view, g_client);
    if (st != OK) {
        fprintf(stderr, "desktop: failed to initialize bridge\n");
        client_close(g_client);
        return 1;
    }

    int fd = client_fd(g_client);
    GIOChannel *channel = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_buffered(channel, FALSE);
    g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_socket_io, g_web_view);
    g_io_channel_unref(channel);

    if (timeout_sec > 0) {
        g_timeout_add_seconds(timeout_sec, on_timeout, NULL);
    }

    webkit_web_view_load_uri(g_web_view, uri);
    gtk_widget_show_all(window);

    gtk_main();

    client_close(g_client);
    g_client = NULL;
    return 0;
}
