#define _POSIX_C_SOURCE 200809L
#include "bridge.h"
#include "net_session.h"
#include "tcp_client.h"
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DEFAULT_PORT = 9090,
    WINDOW_WIDTH = 1024,
    WINDOW_HEIGHT = 768,
    /* The server drops a connection after 15000 ms without a frame, so a
     * PING every 5000 ms keeps an idle client alive with room for loss. */
    PING_INTERVAL_MS = 5000,
    /* Mirror of the server's window: if nothing (not even a PING reply) has
     * arrived for this long, the link is dead even if TCP has not noticed. */
    HEARTBEAT_TIMEOUT_MS = 15000,
    /* A host that drops SYNs never answers; give up on the attempt after
     * this long and let the backoff schedule the next one. */
    CONNECT_TIMEOUT_MS = 3000,
    /* --loop-probe diagnostics: tick often, report gaps a user would feel. */
    LOOP_PROBE_INTERVAL_MS = 100,
    LOOP_STALL_REPORT_MS = 500,
    NO_SOURCE = 0
};

static Client *g_client = NULL;
static NetSession *g_session = NULL;
static WebKitWebView *g_web_view = NULL;
static GIOChannel *g_io_channel = NULL;
static guint g_io_watch_id = NO_SOURCE;
static guint g_ping_timer_id = NO_SOURCE;
static guint g_reconnect_timer_id = NO_SOURCE;
static gint64 g_last_rx_ms = 0;
static const char *g_host = "127.0.0.1";
static uint16_t g_port = DEFAULT_PORT;
/* Replayed when a page finishes loading, since events sent earlier are lost. */
static const char *g_last_status = NULL;
static bool g_quitting = false;
static gint64 g_probe_last_ms = 0;
static gint64 g_probe_max_gap_ms = 0;
/* Resolved once at startup so reconnects never wait on DNS. */
static ClientAddress g_address;
/* The pending non-blocking connect, if any (fd -1 when none). */
static int g_connect_fd = -1;
static GIOChannel *g_connect_channel = NULL;
static guint g_connect_watch_id = NO_SOURCE;
static guint g_connect_timeout_id = NO_SOURCE;

static gboolean on_socket_io(GIOChannel *source, GIOCondition condition, gpointer data);

static gint64 now_ms(void)
{
    /* Monotonic so a wall-clock change cannot fake or hide a heartbeat gap. */
    return g_get_monotonic_time() / G_TIME_SPAN_MILLISECOND;
}

static void emit_connection_state(json_object *payload)
{
    Envelope event = {1, 0, "CONNECTION_STATE", payload};
    bridge_emit(g_web_view, &event);
}

static void emit_status(const char *status)
{
    g_last_status = status;
    json_object *payload = json_object_new_object();
    if (payload == NULL) {
        return;
    }
    json_object_object_add(payload, "status", json_object_new_string(status));
    emit_connection_state(payload);
    json_object_put(payload);
}

static void update_socket_watch(void)
{
    if (g_io_watch_id != NO_SOURCE) {
        g_source_remove(g_io_watch_id);
        g_io_watch_id = NO_SOURCE;
    }
    if (!g_io_channel || !g_client) return;

    GIOCondition cond = G_IO_IN | G_IO_HUP | G_IO_ERR;
    /* Only wait for writability while a partial send is pending, otherwise
     * the always-writable socket would spin the main loop. */
    if (g_client->conn.out_len > g_client->conn.out_pos) {
        cond |= G_IO_OUT;
    }
    g_io_watch_id = g_io_add_watch(g_io_channel, cond, on_socket_io, NULL);
}

static void stop_socket_watch(void)
{
    if (g_io_watch_id != NO_SOURCE) {
        g_source_remove(g_io_watch_id);
        g_io_watch_id = NO_SOURCE;
    }
    if (g_io_channel) {
        g_io_channel_unref(g_io_channel);
        g_io_channel = NULL;
    }
}

static void on_client_pending_write(void *ctx, bool has_pending)
{
    (void)ctx;
    (void)has_pending;
    update_socket_watch();
}

static gboolean on_reconnect_timer(gpointer data);

static void schedule_reconnect(void)
{
    if (g_quitting || g_reconnect_timer_id != NO_SOURCE || g_connect_fd >= 0) return;
    uint32_t delayMs = ns_next_backoff_ms(g_session);
    fprintf(stderr, "desktop: reconnecting to %s:%u in %u ms\n", g_host, g_port, delayMs);
    g_reconnect_timer_id = g_timeout_add(delayMs, on_reconnect_timer, NULL);
}

/* Tear down the dead connection but keep the window and the session token,
 * so the next connection can RESUME the same seat. */
static void handle_disconnect(const char *reason)
{
    if (!g_client) return;
    fprintf(stderr, "desktop: connection lost (%s)\n", reason);
    stop_socket_watch();
    client_close(g_client);
    g_client = NULL;
    ns_on_disconnected(g_session);
    emit_status("reconnecting");
    schedule_reconnect();
}

/* Send a request whose response is consumed natively (PING, RESUME). */
static Status send_native(int32_t wireId, const char *type, json_object *payload)
{
    json_object *body = payload ? json_object_get(payload) : json_object_new_object();
    if (body == NULL) return BUSY;
    Envelope request = {1, wireId, type, body};
    Status s = client_send(g_client, &request);
    json_object_put(body);
    return s;
}

static Status send_resume(void)
{
    int32_t wireId = ns_internal(g_session, "RESUME");
    if (wireId == 0) return BUSY;
    json_object *payload = json_object_new_object();
    if (payload == NULL) return BUSY;
    json_object_object_add(payload, "token", json_object_new_string(ns_token(g_session)));
    Status s = send_native(wireId, "RESUME", payload);
    json_object_put(payload);
    return s;
}

static Status on_client_frame(const Envelope *e, void *ctx)
{
    (void)ctx;
    g_last_rx_ms = now_ms();

    int32_t jsId = 0;
    json_object *connState = NULL;
    NsVerdict verdict = ns_incoming(g_session, e, &jsId, &connState);
    if (connState != NULL) {
        bool expired = json_object_object_get(connState, "sessionExpired") != NULL;
        fprintf(stderr, "desktop: %s\n", expired ? "session expired" : "session resumed");
        g_last_status = "connected";
        emit_connection_state(connState);
        json_object_put(connState);
    }
    if (verdict == NS_FORWARD) {
        Envelope forwarded = *e;
        forwarded.id = jsId;
        bridge_emit(g_web_view, &forwarded);
    }
    return OK;
}

static gboolean on_socket_io(GIOChannel *source, GIOCondition condition, gpointer data)
{
    (void)source;
    (void)data;
    if (!g_client) return G_SOURCE_REMOVE;

    /* Read before checking HUP so frames sent just before a close still
     * reach JS; a closed peer then shows up as a read failure. */
    if (condition & G_IO_IN) {
        if (client_read(g_client, on_client_frame, NULL) != OK) {
            handle_disconnect("closed by peer or read error");
            return G_SOURCE_REMOVE;
        }
    }

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        handle_disconnect("socket hang-up");
        return G_SOURCE_REMOVE;
    }

    if (condition & G_IO_OUT) {
        if (conn_flush(&g_client->conn) != OK) {
            handle_disconnect("write error");
            return G_SOURCE_REMOVE;
        }
        if (g_client->conn.out_len == g_client->conn.out_pos) {
            /* Re-arm without G_IO_OUT; this source is replaced. */
            update_socket_watch();
            return G_SOURCE_REMOVE;
        }
    }

    return G_SOURCE_CONTINUE;
}

static void connect_failed(const char *reason);

/* Take ownership of a connected client: watch it and start the session
 * (RESUME in flight when a token is held, otherwise "connected"). */
static void attach_client(Client *client)
{
    GIOChannel *channel = g_io_channel_unix_new(client_fd(client));
    /* Raw bytes, unbuffered: framing is done by Conn, not by GIOChannel. */
    if (g_io_channel_set_encoding(channel, NULL, NULL) != G_IO_STATUS_NORMAL) {
        g_io_channel_unref(channel);
        client_close(client);
        connect_failed("socket channel setup failed");
        return;
    }
    g_io_channel_set_buffered(channel, FALSE);

    g_client = client;
    g_io_channel = channel;
    client_set_pending_callback(g_client, on_client_pending_write, NULL);
    update_socket_watch();
    g_last_rx_ms = now_ms();
    ns_on_connected(g_session);
    fprintf(stderr, "desktop: connected to %s:%u\n", g_host, g_port);

    if (ns_state(g_session) == NS_RESUMING) {
        /* JS stays in "reconnecting" until the RESUME answer arrives. */
        if (send_resume() != OK) {
            handle_disconnect("RESUME send failed");
        }
        return;
    }
    emit_status("connected");
}

/* Forget the pending attempt's watch, timer and channel; returns its fd,
 * which the caller now owns (to finish or abort). */
static int detach_connect_attempt(void)
{
    if (g_connect_watch_id != NO_SOURCE) {
        g_source_remove(g_connect_watch_id);
        g_connect_watch_id = NO_SOURCE;
    }
    if (g_connect_timeout_id != NO_SOURCE) {
        g_source_remove(g_connect_timeout_id);
        g_connect_timeout_id = NO_SOURCE;
    }
    if (g_connect_channel) {
        /* The channel does not own the fd (close_on_unref is off). */
        g_io_channel_unref(g_connect_channel);
        g_connect_channel = NULL;
    }
    int fd = g_connect_fd;
    g_connect_fd = -1;
    return fd;
}

static void cancel_connect_attempt(void)
{
    client_connect_abort(detach_connect_attempt());
}

static void connect_failed(const char *reason)
{
    fprintf(stderr, "desktop: failed to connect to %s:%u (%s)\n", g_host, g_port, reason);
    /* The first failure after "connecting" turns the UI to "reconnecting". */
    if (g_last_status == NULL || strcmp(g_last_status, "reconnecting") != 0) {
        emit_status("reconnecting");
    }
    schedule_reconnect();
}

static void finish_connect(int fd)
{
    Client *client = NULL;
    if (client_connect_finish(fd, &client) != OK) {
        connect_failed("refused or unreachable");
        return;
    }
    attach_client(client);
}

static gboolean on_connect_ready(GIOChannel *source, GIOCondition condition, gpointer data)
{
    (void)source;
    (void)condition;
    (void)data;
    /* Writable or error both mean the handshake ended; SO_ERROR tells which.
     * Detaching removes this very source, hence G_SOURCE_REMOVE. */
    finish_connect(detach_connect_attempt());
    return G_SOURCE_REMOVE;
}

static gboolean on_connect_timeout(gpointer data)
{
    (void)data;
    /* This timer is the one being dispatched; forget it before detaching. */
    g_connect_timeout_id = NO_SOURCE;
    cancel_connect_attempt();
    connect_failed("timed out");
    return G_SOURCE_REMOVE;
}

/* Start one attempt without blocking the GTK thread: connect() returns at
 * once and the result arrives through a G_IO_OUT watch or the timeout. */
static void start_connect_attempt(void)
{
    if (g_quitting || g_client || g_connect_fd >= 0) return;

    int fd = -1;
    bool inProgress = false;
    if (client_connect_start(&g_address, &fd, &inProgress) != OK) {
        connect_failed("connect error");
        return;
    }
    if (!inProgress) {
        finish_connect(fd);
        return;
    }
    g_connect_fd = fd;
    g_connect_channel = g_io_channel_unix_new(fd);
    g_connect_watch_id = g_io_add_watch(g_connect_channel, G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                        on_connect_ready, NULL);
    g_connect_timeout_id = g_timeout_add(CONNECT_TIMEOUT_MS, on_connect_timeout, NULL);
}

static gboolean on_reconnect_timer(gpointer data)
{
    (void)data;
    g_reconnect_timer_id = NO_SOURCE;
    start_connect_attempt();
    return G_SOURCE_REMOVE;
}

static gboolean on_heartbeat(gpointer data)
{
    (void)data;
    if (!g_client) return G_SOURCE_CONTINUE;

    if (now_ms() - g_last_rx_ms >= HEARTBEAT_TIMEOUT_MS) {
        handle_disconnect("heartbeat timeout");
        return G_SOURCE_CONTINUE;
    }
    int32_t wireId = ns_internal(g_session, "PING");
    if (wireId == 0) {
        /* No free slot means replies are not coming; the timeout above
         * will catch that, so there is nothing to send now. */
        return G_SOURCE_CONTINUE;
    }
    if (send_native(wireId, "PING", NULL) != OK) {
        handle_disconnect("PING send failed");
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

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event, gpointer data)
{
    (void)web_view;
    (void)data;
    /* The first connect happens before the page exists; tell it now. */
    if (event == WEBKIT_LOAD_FINISHED && g_last_status != NULL) {
        emit_status(g_last_status);
    }
}

static void request_quit(void)
{
    g_quitting = true;
    gtk_main_quit();
}

static void on_window_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    /* The view dies with the window; later emits must not touch it. */
    g_web_view = NULL;
    fprintf(stderr, "desktop: window closed\n");
    request_quit();
}

static void on_bridge_disconnect(void *ctx)
{
    (void)ctx;
    handle_disconnect("send failed");
}

static void on_spike_done(void *ctx)
{
    (void)ctx;
    request_quit();
}

static void report_loop_probe(void)
{
    if (g_probe_last_ms != 0) {
        fprintf(stderr, "desktop: loop probe max gap %" G_GINT64_FORMAT " ms\n", g_probe_max_gap_ms);
    }
}

/* Diagnostic for --loop-probe: a timer that should fire every 100 ms; a
 * longer gap means something blocked the GTK thread (and froze the UI). */
static gboolean on_loop_probe(gpointer data)
{
    (void)data;
    gint64 nowMs = now_ms();
    gint64 gapMs = nowMs - g_probe_last_ms;
    if (gapMs > g_probe_max_gap_ms) {
        g_probe_max_gap_ms = gapMs;
    }
    if (gapMs > LOOP_STALL_REPORT_MS) {
        fprintf(stderr, "desktop: main loop stalled for %" G_GINT64_FORMAT " ms\n", gapMs);
    }
    g_probe_last_ms = nowMs;
    return G_SOURCE_CONTINUE;
}

static gboolean on_timeout(gpointer data)
{
    (void)data;
    report_loop_probe();
    fprintf(stderr, "desktop: safety timeout reached\n");
    gtk_main_quit();
    exit(1);
    return G_SOURCE_REMOVE;
}

/* Remove every source that could touch the client before closing it. */
static void shutdown_network(void)
{
    g_quitting = true;
    /* Closing during a pending connect: drop its watch, timer and fd too. */
    if (g_connect_fd >= 0) {
        fprintf(stderr, "desktop: cancelled pending connect on shutdown\n");
    }
    cancel_connect_attempt();
    stop_socket_watch();
    if (g_ping_timer_id != NO_SOURCE) {
        g_source_remove(g_ping_timer_id);
        g_ping_timer_id = NO_SOURCE;
    }
    if (g_reconnect_timer_id != NO_SOURCE) {
        g_source_remove(g_reconnect_timer_id);
        g_reconnect_timer_id = NO_SOURCE;
    }
    if (g_client) {
        client_close(g_client);
        g_client = NULL;
    }
    ns_destroy(g_session);
    g_session = NULL;
}

int main(int argc, char *argv[])
{
    const char *assets = "./client/dist";
    const char *uri = "uno://app/index.html";
    int timeout_sec = 0;
    bool loopProbe = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            g_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--loop-probe") == 0) {
            loopProbe = true;
        }
    }

    bridge_set_assets_dir(assets);
    bridge_set_done_callback(on_spike_done, NULL);
    bridge_set_disconnect_callback(on_bridge_disconnect, NULL);

    /* The host is fixed for the process lifetime, so resolve it once here,
     * before any window exists; an IP needs no DNS at all. A name that does
     * not resolve is a configuration error, not a network outage. */
    if (client_resolve(g_host, g_port, &g_address) != OK) {
        fprintf(stderr, "desktop: cannot resolve %s:%u\n", g_host, g_port);
        return 1;
    }

    gtk_init(&argc, &argv);

    g_session = ns_create();
    if (!g_session) {
        fprintf(stderr, "desktop: out of memory\n");
        return 1;
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_window_set_title(GTK_WINDOW(window), "UNO Multiplayer");
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *view_widget = webkit_web_view_new();
    g_web_view = WEBKIT_WEB_VIEW(view_widget);
    g_signal_connect(g_web_view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(g_web_view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    gtk_container_add(GTK_CONTAINER(window), view_widget);

    if (bridge_init(g_web_view, g_session, &g_client) != OK) {
        fprintf(stderr, "desktop: failed to initialize bridge\n");
        ns_destroy(g_session);
        return 1;
    }

    /* A server that is not up yet is not fatal: open the window anyway and
     * keep retrying with backoff, like after a drop. */
    emit_status("connecting");
    start_connect_attempt();
    g_ping_timer_id = g_timeout_add(PING_INTERVAL_MS, on_heartbeat, NULL);

    if (timeout_sec > 0) {
        g_timeout_add_seconds(timeout_sec, on_timeout, NULL);
    }
    if (loopProbe) {
        g_probe_last_ms = now_ms();
        g_timeout_add(LOOP_PROBE_INTERVAL_MS, on_loop_probe, NULL);
    }

    webkit_web_view_load_uri(g_web_view, uri);
    gtk_widget_show_all(window);

    gtk_main();

    report_loop_probe();
    shutdown_network();
    return 0;
}
