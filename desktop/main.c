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

static Client *gClient = NULL;
static NetSession *gSession = NULL;
static WebKitWebView *gWebView = NULL;
static GIOChannel *gIoChannel = NULL;
static guint gIoWatchId = NO_SOURCE;
static guint gPingTimerId = NO_SOURCE;
static guint gReconnectTimerId = NO_SOURCE;
static gint64 gLastReceiveMs = 0;
static const char *gHost = "127.0.0.1";
static uint16_t gPort = DEFAULT_PORT;
/* Replayed when a page finishes loading, since events sent earlier are lost. */
static const char *gLastStatus = NULL;
static bool gQuitting = false;
static gint64 gProbeLastMs = 0;
static gint64 gProbeMaxGapMs = 0;
/* Resolved once at startup so reconnects never wait on DNS. */
static ClientAddress gAddress;
/* The pending non-blocking connect, if any (fd -1 when none). */
static int gConnectFd = -1;
static GIOChannel *gConnectChannel = NULL;
static guint gConnectWatchId = NO_SOURCE;
static guint gConnectTimeoutId = NO_SOURCE;

static gboolean onSocketIo(GIOChannel *source, GIOCondition condition, gpointer data);

static gint64 monotonicNowMs(void) {
    /* Monotonic so a wall-clock change cannot fake or hide a heartbeat gap. */
    return g_get_monotonic_time() / G_TIME_SPAN_MILLISECOND;
}

static void emitConnectionState(json_object *payload) {
    Envelope event = {1, 0, "CONNECTION_STATE", payload};
    bridge_emit(gWebView, &event);
}

static void emitStatus(const char *status) {
    gLastStatus = status;
    json_object *payload = json_object_new_object();
    if (payload == NULL) {
        return;
    }
    json_object_object_add(payload, "status", json_object_new_string(status));
    emitConnectionState(payload);
    json_object_put(payload);
}

static void updateSocketWatch(void) {
    if (gIoWatchId != NO_SOURCE) {
        g_source_remove(gIoWatchId);
        gIoWatchId = NO_SOURCE;
    }
    if (!gIoChannel || !gClient) return;

    GIOCondition cond = G_IO_IN | G_IO_HUP | G_IO_ERR;
    /* Only wait for writability while a partial send is pending, otherwise
     * the always-writable socket would spin the main loop. */
    if (gClient->conn.out_len > gClient->conn.out_pos) {
        cond |= G_IO_OUT;
    }
    gIoWatchId = g_io_add_watch(gIoChannel, cond, onSocketIo, NULL);
}

static void stopSocketWatch(void) {
    if (gIoWatchId != NO_SOURCE) {
        g_source_remove(gIoWatchId);
        gIoWatchId = NO_SOURCE;
    }
    if (gIoChannel) {
        g_io_channel_unref(gIoChannel);
        gIoChannel = NULL;
    }
}

static void onClientPendingWrite(void *context, bool hasPending) {
    (void)context;
    (void)hasPending;
    updateSocketWatch();
}

static gboolean onReconnectTimer(gpointer data);

static void scheduleReconnect(void) {
    if (gQuitting || gReconnectTimerId != NO_SOURCE || gConnectFd >= 0) return;
    uint32_t delayMs = ns_next_backoff_ms(gSession);
    fprintf(stderr, "desktop: reconnecting to %s:%u in %u ms\n", gHost, gPort, delayMs);
    gReconnectTimerId = g_timeout_add(delayMs, onReconnectTimer, NULL);
}

/* Tear down the dead connection but keep the window and the session token,
 * so the next connection can RESUME the same seat. */
static void handleDisconnect(const char *reason) {
    if (!gClient) return;
    fprintf(stderr, "desktop: connection lost (%s)\n", reason);
    stopSocketWatch();
    client_close(gClient);
    gClient = NULL;
    ns_on_disconnected(gSession);
    emitStatus("reconnecting");
    scheduleReconnect();
}

/* Send a request whose response is consumed natively (PING, RESUME). */
static Status sendNative(int32_t wireId, const char *type, json_object *payload) {
    json_object *body = payload ? json_object_get(payload) : json_object_new_object();
    if (body == NULL) return BUSY;
    Envelope request = {1, wireId, type, body};
    Status status = client_send(gClient, &request);
    json_object_put(body);
    return status;
}

static Status sendResume(void) {
    int32_t wireId = ns_internal(gSession, "RESUME");
    if (wireId == 0) return BUSY;
    json_object *payload = json_object_new_object();
    if (payload == NULL) return BUSY;
    json_object_object_add(payload, "token", json_object_new_string(ns_token(gSession)));
    Status status = sendNative(wireId, "RESUME", payload);
    json_object_put(payload);
    return status;
}

static Status onClientFrame(const Envelope *envelope, void *context) {
    (void)context;
    gLastReceiveMs = monotonicNowMs();

    int32_t jsId = 0;
    json_object *connState = NULL;
    NsVerdict verdict = ns_incoming(gSession, envelope, &jsId, &connState);
    if (connState != NULL) {
        bool expired = json_object_object_get(connState, "sessionExpired") != NULL;
        fprintf(stderr, "desktop: %s\n", expired ? "session expired" : "session resumed");
        gLastStatus = "connected";
        emitConnectionState(connState);
        json_object_put(connState);
    }
    if (verdict == NS_FORWARD) {
        Envelope forwarded = *envelope;
        forwarded.id = jsId;
        bridge_emit(gWebView, &forwarded);
    }
    return OK;
}

static gboolean onSocketIo(GIOChannel *source, GIOCondition condition, gpointer data) {
    (void)source;
    (void)data;
    if (!gClient) return G_SOURCE_REMOVE;

    /* Read before checking HUP so frames sent just before a close still
     * reach JS; a closed peer then shows up as a read failure. */
    if (condition & G_IO_IN) {
        if (client_read(gClient, onClientFrame, NULL) != OK) {
            handleDisconnect("closed by peer or read error");
            return G_SOURCE_REMOVE;
        }
    }

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        handleDisconnect("socket hang-up");
        return G_SOURCE_REMOVE;
    }

    if (condition & G_IO_OUT) {
        if (conn_flush(&gClient->conn) != OK) {
            handleDisconnect("write error");
            return G_SOURCE_REMOVE;
        }
        if (gClient->conn.out_len == gClient->conn.out_pos) {
            /* Re-arm without G_IO_OUT; this source is replaced. */
            updateSocketWatch();
            return G_SOURCE_REMOVE;
        }
    }

    return G_SOURCE_CONTINUE;
}

static void connectFailed(const char *reason);

/* Take ownership of a connected client: watch it and start the session
 * (RESUME in flight when a token is held, otherwise "connected"). */
static void attachClient(Client *client) {
    GIOChannel *channel = g_io_channel_unix_new(client_fd(client));
    /* Raw bytes, unbuffered: framing is done by Conn, not by GIOChannel. */
    if (g_io_channel_set_encoding(channel, NULL, NULL) != G_IO_STATUS_NORMAL) {
        g_io_channel_unref(channel);
        client_close(client);
        connectFailed("socket channel setup failed");
        return;
    }
    g_io_channel_set_buffered(channel, FALSE);

    gClient = client;
    gIoChannel = channel;
    client_set_pending_callback(gClient, onClientPendingWrite, NULL);
    updateSocketWatch();
    gLastReceiveMs = monotonicNowMs();
    ns_on_connected(gSession);
    fprintf(stderr, "desktop: connected to %s:%u\n", gHost, gPort);

    if (ns_state(gSession) == NS_RESUMING) {
        /* JS stays in "reconnecting" until the RESUME answer arrives. */
        if (sendResume() != OK) {
            handleDisconnect("RESUME send failed");
        }
        return;
    }
    emitStatus("connected");
}

/* Forget the pending attempt's watch, timer and channel; returns its fd,
 * which the caller now owns (to finish or abort). */
static int detachConnectAttempt(void) {
    if (gConnectWatchId != NO_SOURCE) {
        g_source_remove(gConnectWatchId);
        gConnectWatchId = NO_SOURCE;
    }
    if (gConnectTimeoutId != NO_SOURCE) {
        g_source_remove(gConnectTimeoutId);
        gConnectTimeoutId = NO_SOURCE;
    }
    if (gConnectChannel) {
        /* The channel does not own the fd (close_on_unref is off). */
        g_io_channel_unref(gConnectChannel);
        gConnectChannel = NULL;
    }
    int connectionSocket = gConnectFd;
    gConnectFd = -1;
    return connectionSocket;
}

static void cancelConnectAttempt(void) {
    client_connect_abort(detachConnectAttempt());
}

static void connectFailed(const char *reason) {
    fprintf(stderr, "desktop: failed to connect to %s:%u (%s)\n", gHost, gPort, reason);
    /* The first failure after "connecting" turns the UI to "reconnecting". */
    if (gLastStatus == NULL || strcmp(gLastStatus, "reconnecting") != 0) {
        emitStatus("reconnecting");
    }
    scheduleReconnect();
}

static void finishConnect(int connectionSocket) {
    Client *client = NULL;
    if (client_connect_finish(connectionSocket, &client) != OK) {
        connectFailed("refused or unreachable");
        return;
    }
    attachClient(client);
}

static gboolean onConnectReady(GIOChannel *source, GIOCondition condition, gpointer data) {
    (void)source;
    (void)condition;
    (void)data;
    /* Writable or error both mean the handshake ended; SO_ERROR tells which.
     * Detaching removes this very source, hence G_SOURCE_REMOVE. */
    finishConnect(detachConnectAttempt());
    return G_SOURCE_REMOVE;
}

static gboolean onConnectTimeout(gpointer data) {
    (void)data;
    /* This timer is the one being dispatched; forget it before detaching. */
    gConnectTimeoutId = NO_SOURCE;
    cancelConnectAttempt();
    connectFailed("timed out");
    return G_SOURCE_REMOVE;
}

/* Start one attempt without blocking the GTK thread: connect() returns at
 * once and the result arrives through a G_IO_OUT watch or the timeout. */
static void startConnectAttempt(void) {
    if (gQuitting || gClient || gConnectFd >= 0) return;

    int connectionSocket = -1;
    bool inProgress = false;
    if (client_connect_start(&gAddress, &connectionSocket, &inProgress) != OK) {
        connectFailed("connect error");
        return;
    }
    if (!inProgress) {
        finishConnect(connectionSocket);
        return;
    }
    gConnectFd = connectionSocket;
    gConnectChannel = g_io_channel_unix_new(connectionSocket);
    gConnectWatchId = g_io_add_watch(gConnectChannel, G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                        onConnectReady, NULL);
    gConnectTimeoutId = g_timeout_add(CONNECT_TIMEOUT_MS, onConnectTimeout, NULL);
}

static gboolean onReconnectTimer(gpointer data) {
    (void)data;
    gReconnectTimerId = NO_SOURCE;
    startConnectAttempt();
    return G_SOURCE_REMOVE;
}

/* Runs every PING_INTERVAL_MS (5000 ms): the server drops a connection after
 * 15000 ms without a frame, and no reply for HEARTBEAT_TIMEOUT_MS means the link
 * is dead even if TCP has not noticed yet. */
static gboolean onHeartbeat(gpointer data) {
    (void)data;
    if (!gClient) return G_SOURCE_CONTINUE;

    if (monotonicNowMs() - gLastReceiveMs >= HEARTBEAT_TIMEOUT_MS) {
        handleDisconnect("heartbeat timeout");
        return G_SOURCE_CONTINUE;
    }
    int32_t wireId = ns_internal(gSession, "PING");
    if (wireId == 0) {
        /* No free slot means replies are not coming; the timeout above
         * will catch that, so there is nothing to send now. */
        return G_SOURCE_CONTINUE;
    }
    if (sendNative(wireId, "PING", NULL) != OK) {
        handleDisconnect("PING send failed");
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

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent event, gpointer data) {
    (void)web_view;
    (void)data;
    /* The first connect happens before the page exists; tell it now. */
    if (event == WEBKIT_LOAD_FINISHED && gLastStatus != NULL) {
        emitStatus(gLastStatus);
    }
}

static void requestQuit(void) {
    gQuitting = true;
    gtk_main_quit();
}

static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    /* The view dies with the window; later emits must not touch it. */
    gWebView = NULL;
    fprintf(stderr, "desktop: window closed\n");
    requestQuit();
}

static void onBridgeDisconnect(void *context) {
    (void)context;
    handleDisconnect("send failed");
}

static void onSpikeDone(void *context) {
    (void)context;
    requestQuit();
}

static void reportLoopProbe(void) {
    if (gProbeLastMs != 0) {
        fprintf(stderr, "desktop: loop probe max gap %" G_GINT64_FORMAT " ms\n", gProbeMaxGapMs);
    }
}

/* Diagnostic for --loop-probe: a timer that should fire every 100 ms; a
 * longer gap means something blocked the GTK thread (and froze the UI). */
static gboolean onLoopProbe(gpointer data) {
    (void)data;
    gint64 nowMs = monotonicNowMs();
    gint64 gapMs = nowMs - gProbeLastMs;
    if (gapMs > gProbeMaxGapMs) {
        gProbeMaxGapMs = gapMs;
    }
    if (gapMs > LOOP_STALL_REPORT_MS) {
        fprintf(stderr, "desktop: main loop stalled for %" G_GINT64_FORMAT " ms\n", gapMs);
    }
    gProbeLastMs = nowMs;
    return G_SOURCE_CONTINUE;
}

static gboolean onTimeout(gpointer data) {
    (void)data;
    reportLoopProbe();
    fprintf(stderr, "desktop: safety timeout reached\n");
    gtk_main_quit();
    exit(1);
    return G_SOURCE_REMOVE;
}

/* Remove every source that could touch the client before closing it. */
static void shutdownNetwork(void) {
    gQuitting = true;
    /* Closing during a pending connect: drop its watch, timer and fd too. */
    if (gConnectFd >= 0) {
        fprintf(stderr, "desktop: cancelled pending connect on shutdown\n");
    }
    cancelConnectAttempt();
    stopSocketWatch();
    if (gPingTimerId != NO_SOURCE) {
        g_source_remove(gPingTimerId);
        gPingTimerId = NO_SOURCE;
    }
    if (gReconnectTimerId != NO_SOURCE) {
        g_source_remove(gReconnectTimerId);
        gReconnectTimerId = NO_SOURCE;
    }
    if (gClient) {
        client_close(gClient);
        gClient = NULL;
    }
    ns_destroy(gSession);
    gSession = NULL;
}

int main(int argc, char *argv[]) {
    const char *assets = "./client/dist";
    const char *uri = "uno://app/index.html";
    int timeoutSec = 0;
    bool loopProbe = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            gHost = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            gPort = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeoutSec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--loop-probe") == 0) {
            loopProbe = true;
        }
    }

    bridge_set_assets_dir(assets);
    bridge_set_done_callback(onSpikeDone, NULL);
    bridge_set_disconnect_callback(onBridgeDisconnect, NULL);

    /* The host is fixed for the process lifetime, so resolve it once here,
     * before any window exists; an IP needs no DNS at all. A name that does
     * not resolve is a configuration error, not a network outage. */
    if (client_resolve(gHost, gPort, &gAddress) != OK) {
        fprintf(stderr, "desktop: cannot resolve %s:%u\n", gHost, gPort);
        return 1;
    }

    gtk_init(&argc, &argv);

    gSession = ns_create();
    if (!gSession) {
        fprintf(stderr, "desktop: out of memory\n");
        return 1;
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_window_set_title(GTK_WINDOW(window), "UNO Multiplayer");
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *view_widget = webkit_web_view_new();
    gWebView = WEBKIT_WEB_VIEW(view_widget);
    g_signal_connect(gWebView, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(gWebView, "load-changed", G_CALLBACK(on_load_changed), NULL);
    gtk_container_add(GTK_CONTAINER(window), view_widget);

    if (bridge_init(gWebView, gSession, &gClient) != OK) {
        fprintf(stderr, "desktop: failed to initialize bridge\n");
        ns_destroy(gSession);
        return 1;
    }

    /* A server that is not up yet is not fatal: open the window anyway and
     * keep retrying with backoff, like after a drop. */
    emitStatus("connecting");
    startConnectAttempt();
    gPingTimerId = g_timeout_add(PING_INTERVAL_MS, onHeartbeat, NULL);

    if (timeoutSec > 0) {
        g_timeout_add_seconds(timeoutSec, onTimeout, NULL);
    }
    if (loopProbe) {
        gProbeLastMs = monotonicNowMs();
        g_timeout_add(LOOP_PROBE_INTERVAL_MS, onLoopProbe, NULL);
    }

    webkit_web_view_load_uri(gWebView, uri);
    gtk_widget_show_all(window);

    gtk_main();

    reportLoopProbe();
    shutdownNetwork();
    return 0;
}
