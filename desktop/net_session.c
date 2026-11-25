#include "desktop/net_session.h"
#include <stdlib.h>
#include <string.h>

enum {
    BACKOFF_INITIAL_MS = 1000,
    BACKOFF_MAX_MS = 8000,
    NO_WIRE_ID = 0 /* id 0 is reserved for server pushes */
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
    char *token;
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
    free(session->token);
    session->token = NULL;
}

/* Copies the token so it outlives the response envelope it came from. */
static void store_token(NetSession *session, const char *token)
{
    size_t length = strlen(token);
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        /* Without a token the session simply cannot resume later. */
        clear_token(session);
        return;
    }
    memcpy(copy, token, length + 1);
    clear_token(session);
    session->token = copy;
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
    s->backoffMs = 0;
    clear_pending(s);
    s->state = s->token != NULL ? NS_RESUMING : NS_CONNECTED;
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
    if (s == NULL || type == NULL || wire_id == NULL) {
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
    return s != NULL && s->token != NULL;
}

const char *ns_token(const NetSession *s)
{
    return s != NULL ? s->token : NULL;
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
