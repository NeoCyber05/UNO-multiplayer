#include "desktop/net_session.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

enum {
    BACKOFF_INITIAL_MS = 1000,
    BACKOFF_MAX_MS = 8000,
    NO_WIRE_ID = 0, /* id 0 is reserved for server pushes */
    /* Server tokens are 64 hex chars; the margin tolerates a longer format. */
    TOKEN_CAPACITY = 128
};

/* Only these request types change native state when their response arrives. */
typedef enum {
    REQUEST_OTHER,
    REQUEST_LOGIN,
    REQUEST_LOGOUT,
    REQUEST_RESUME
} RequestKind;

typedef struct {
    bool used;
    bool internal; /* sent by native code: response is never forwarded */
    int32_t wireId;
    int32_t jsId;
    RequestKind kind;
} PendingRequest;

struct NetSession {
    NsState state;
    int32_t lastWireId;
    uint32_t backoffMs;
    /* Stored inline rather than on the heap so wiping it is the only way it
     * ever goes away: no freed allocation can keep a copy of the secret. */
    bool hasToken;
    char token[TOKEN_CAPACITY];
    PendingRequest pending[NS_MAX_PENDING];
};

static RequestKind request_kind(const char *type)
{
    if (strcmp(type, "LOGIN") == 0) {
        return REQUEST_LOGIN;
    }
    if (strcmp(type, "LOGOUT") == 0) {
        return REQUEST_LOGOUT;
    }
    if (strcmp(type, "RESUME") == 0) {
        return REQUEST_RESUME;
    }
    return REQUEST_OTHER;
}

static void clear_pending(NetSession *session)
{
    memset(session->pending, 0, sizeof(session->pending));
}

static void clear_token(NetSession *session)
{
    /* sodium_memzero cannot be optimised away like a memset before free. */
    sodium_memzero(session->token, sizeof(session->token));
    session->hasToken = false;
}

/* Copies the token so it outlives the response envelope it came from. */
static void store_token(NetSession *session, const char *token)
{
    size_t length = strlen(token);
    clear_token(session);
    if (length == 0 || length >= sizeof(session->token)) {
        /* Without a token the session simply cannot resume later. */
        return;
    }
    memcpy(session->token, token, length + 1);
    session->hasToken = true;
}

/* Registers a request; returns its wire id or NO_WIRE_ID if none is free. */
static int32_t register_request(NetSession *session, int32_t jsId, RequestKind kind, bool internal)
{
    /* The server rejects non-increasing ids, so wrapping is not an option. */
    if (session->lastWireId == INT32_MAX) {
        return NO_WIRE_ID;
    }
    for (size_t index = 0; index < NS_MAX_PENDING; index++) {
        PendingRequest *slot = &session->pending[index];
        if (!slot->used) {
            session->lastWireId++;
            slot->used = true;
            slot->internal = internal;
            slot->wireId = session->lastWireId;
            slot->jsId = jsId;
            slot->kind = kind;
            return slot->wireId;
        }
    }
    return NO_WIRE_ID;
}

static PendingRequest *find_pending(NetSession *session, int32_t wireId)
{
    for (size_t index = 0; index < NS_MAX_PENDING; index++) {
        PendingRequest *slot = &session->pending[index];
        if (slot->used && slot->wireId == wireId) {
            return slot;
        }
    }
    return NULL;
}

static bool is_ok_response(const Envelope *e)
{
    return e->type != NULL && strcmp(e->type, "OK") == 0;
}

static json_object *resume_conn_state(const Envelope *e, bool resumed)
{
    json_object *connState = json_object_new_object();
    if (connState == NULL) {
        return NULL;
    }
    json_object_object_add(connState, "status", json_object_new_string("connected"));
    if (!resumed) {
        json_object_object_add(connState, "sessionExpired", json_object_new_boolean(true));
        return connState;
    }
    json_object *me = NULL;
    if (e->payload != NULL && json_object_object_get_ex(e->payload, "me", &me)) {
        /* Take a reference so `me` survives the caller freeing the envelope. */
        json_object_object_add(connState, "me", json_object_get(me));
    }
    return connState;
}

static json_object *finish_resume(NetSession *session, const Envelope *e)
{
    bool resumed = is_ok_response(e);
    if (!resumed) {
        /* The server no longer accepts this token; JS must log in again. */
        clear_token(session);
    }
    session->state = NS_CONNECTED;
    /* Any RESUME answer proves the server is really serving us. */
    session->backoffMs = 0;
    return resume_conn_state(e, resumed);
}

static void apply_js_response(NetSession *session, RequestKind kind, const Envelope *e)
{
    if (!is_ok_response(e)) {
        return;
    }
    if (kind == REQUEST_LOGIN) {
        json_object *token = NULL;
        if (e->payload != NULL && json_object_object_get_ex(e->payload, "token", &token)
            && json_object_is_type(token, json_type_string)) {
            store_token(session, json_object_get_string(token));
        }
    } else if (kind == REQUEST_LOGOUT) {
        clear_token(session);
    }
}

NetSession *ns_create(void)
{
    NetSession *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }
    session->state = NS_DISCONNECTED;
    return session;
}

void ns_destroy(NetSession *s)
{
    if (s == NULL) {
        return;
    }
    clear_token(s);
    free(s);
}

void ns_on_connected(NetSession *s)
{
    if (s == NULL) {
        return;
    }
    s->lastWireId = NO_WIRE_ID;
    clear_pending(s);
    s->state = s->hasToken ? NS_RESUMING : NS_CONNECTED;
    /* While resuming the backoff is kept: a server that accepts and then
     * drops us before answering must not be retried every second. */
    if (s->state == NS_CONNECTED) {
        s->backoffMs = 0;
    }
}

void ns_on_disconnected(NetSession *s)
{
    if (s == NULL) {
        return;
    }
    s->state = NS_DISCONNECTED;
    clear_pending(s);
}

NsState ns_state(const NetSession *s)
{
    return s != NULL ? s->state : NS_DISCONNECTED;
}

Status ns_outgoing(NetSession *s, int32_t js_id, const char *type, int32_t *wire_id)
{
    /* id 0 marks pushes and native requests, so a JS id must be positive or
     * its response could not be told apart from them. */
    if (s == NULL || type == NULL || wire_id == NULL || js_id <= 0) {
        return BAD_REQUEST;
    }
    RequestKind kind = request_kind(type);
    if (kind == REQUEST_RESUME) {
        return BAD_REQUEST;
    }
    /* While RESUMING, JS requests would run unauthenticated; hold them off. */
    if (s->state != NS_CONNECTED) {
        return BUSY;
    }
    int32_t allocated = register_request(s, js_id, kind, false);
    if (allocated == NO_WIRE_ID) {
        return BUSY;
    }
    *wire_id = allocated;
    return OK;
}

int32_t ns_internal(NetSession *s, const char *type)
{
    if (s == NULL || type == NULL) {
        return NO_WIRE_ID;
    }
    if (s->state != NS_RESUMING && s->state != NS_CONNECTED) {
        return NO_WIRE_ID;
    }
    return register_request(s, 0, request_kind(type), true);
}

NsVerdict ns_incoming(NetSession *s, const Envelope *e, int32_t *js_id, json_object **conn_state)
{
    if (js_id != NULL) {
        *js_id = 0;
    }
    if (conn_state != NULL) {
        *conn_state = NULL;
    }
    if (s == NULL || e == NULL) {
        return NS_DROP;
    }
    if (e->id == NO_WIRE_ID) {
        return NS_FORWARD;
    }
    PendingRequest *slot = find_pending(s, e->id);
    if (slot == NULL) {
        /* Duplicate or late response from an earlier connection. */
        return NS_DROP;
    }
    PendingRequest request = *slot;
    slot->used = false;

    if (request.internal) {
        if (request.kind == REQUEST_RESUME) {
            json_object *connState = finish_resume(s, e);
            if (conn_state != NULL) {
                *conn_state = connState;
            } else {
                json_object_put(connState);
            }
        }
        return NS_DROP;
    }
    apply_js_response(s, request.kind, e);
    if (js_id != NULL) {
        *js_id = request.jsId;
    }
    return NS_FORWARD;
}

bool ns_has_token(const NetSession *s)
{
    return s != NULL && s->hasToken;
}

const char *ns_token(const NetSession *s)
{
    return ns_has_token(s) ? s->token : NULL;
}

uint32_t ns_next_backoff_ms(NetSession *s)
{
    if (s == NULL) {
        return 0;
    }
    if (s->backoffMs == 0) {
        s->backoffMs = BACKOFF_INITIAL_MS;
    } else if (s->backoffMs < BACKOFF_MAX_MS) {
        s->backoffMs *= 2;
        if (s->backoffMs > BACKOFF_MAX_MS) {
            s->backoffMs = BACKOFF_MAX_MS;
        }
    }
    return s->backoffMs;
}
