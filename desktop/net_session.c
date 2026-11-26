#include "net_session.h"
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

static RequestKind requestKind(const char *type) {
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

static void clearPending(NetSession *session) {
    memset(session->pending, 0, sizeof(session->pending));
}

static void clearToken(NetSession *session) {
    /* sodium_memzero cannot be optimised away like a memset before free. */
    sodium_memzero(session->token, sizeof(session->token));
    session->hasToken = false;
}

/* Copies the token so it outlives the response envelope it came from. */
static void storeToken(NetSession *session, const char *token) {
    size_t length = strlen(token);
    clearToken(session);
    if (length == 0 || length >= sizeof(session->token)) {
        /* Without a token the session simply cannot resume later. */
        return;
    }
    memcpy(session->token, token, length + 1);
    session->hasToken = true;
}

/* Registers a request; returns its wire id or NO_WIRE_ID if none is free. */
static int32_t registerRequest(NetSession *session, int32_t jsId, RequestKind kind, bool internal) {
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

static PendingRequest *findPending(NetSession *session, int32_t wireId) {
    for (size_t index = 0; index < NS_MAX_PENDING; index++) {
        PendingRequest *slot = &session->pending[index];
        if (slot->used && slot->wireId == wireId) {
            return slot;
        }
    }
    return NULL;
}

static bool isOkResponse(const Envelope *envelope) {
    return envelope->type != NULL && strcmp(envelope->type, "OK") == 0;
}

static json_object *resumeConnState(const Envelope *envelope, bool resumed) {
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
    if (envelope->payload != NULL && json_object_object_get_ex(envelope->payload, "me", &me)) {
        /* Take a reference so `me` survives the caller freeing the envelope. */
        json_object_object_add(connState, "me", json_object_get(me));
    }
    return connState;
}

static json_object *finishResume(NetSession *session, const Envelope *envelope) {
    bool resumed = isOkResponse(envelope);
    if (!resumed) {
        /* The server no longer accepts this token; JS must log in again. */
        clearToken(session);
    }
    session->state = NS_CONNECTED;
    /* Any RESUME answer proves the server is really serving us. */
    session->backoffMs = 0;
    return resumeConnState(envelope, resumed);
}

static void applyJsResponse(NetSession *session, RequestKind kind, const Envelope *envelope) {
    if (!isOkResponse(envelope)) {
        return;
    }
    if (kind == REQUEST_LOGIN) {
        json_object *token = NULL;
        if (envelope->payload != NULL && json_object_object_get_ex(envelope->payload, "token",
                                                                   &token)
            && json_object_is_type(token, json_type_string)) {
            storeToken(session, json_object_get_string(token));
        }
    } else if (kind == REQUEST_LOGOUT) {
        clearToken(session);
    }
}

NetSession *nsCreate(void) {
    NetSession *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }
    session->state = NS_DISCONNECTED;
    return session;
}

void nsDestroy(NetSession *session) {
    if (session == NULL) {
        return;
    }
    clearToken(session);
    free(session);
}

void nsOnConnected(NetSession *session) {
    if (session == NULL) {
        return;
    }
    session->lastWireId = NO_WIRE_ID;
    clearPending(session);
    session->state = session->hasToken ? NS_RESUMING : NS_CONNECTED;
    /* While resuming the backoff is kept: a server that accepts and then
     * drops us before answering must not be retried every second. */
    if (session->state == NS_CONNECTED) {
        session->backoffMs = 0;
    }
}

void nsOnDisconnected(NetSession *session) {
    if (session == NULL) {
        return;
    }
    session->state = NS_DISCONNECTED;
    clearPending(session);
}

NsState nsState(const NetSession *session) {
    return session != NULL ? session->state : NS_DISCONNECTED;
}

Status nsOutgoing(NetSession *session, int32_t jsId, const char *type, int32_t *wireId) {
    /* id 0 marks pushes and native requests, so a JS id must be positive or
     * its response could not be told apart from them. */
    if (session == NULL || type == NULL || wireId == NULL || jsId <= 0) {
        return BAD_REQUEST;
    }
    RequestKind kind = requestKind(type);
    if (kind == REQUEST_RESUME) {
        return BAD_REQUEST;
    }
    /* While RESUMING, JS requests would run unauthenticated; hold them off. */
    if (session->state != NS_CONNECTED) {
        return BUSY;
    }
    int32_t allocated = registerRequest(session, jsId, kind, false);
    if (allocated == NO_WIRE_ID) {
        return BUSY;
    }
    *wireId = allocated;
    return OK;
}

int32_t nsInternal(NetSession *session, const char *type) {
    if (session == NULL || type == NULL) {
        return NO_WIRE_ID;
    }
    if (session->state != NS_RESUMING && session->state != NS_CONNECTED) {
        return NO_WIRE_ID;
    }
    return registerRequest(session, 0, requestKind(type), true);
}

NsVerdict nsIncoming(NetSession *session, const Envelope *envelope, int32_t *jsId,
                      json_object **outConnState) {
    if (jsId != NULL) {
        *jsId = 0;
    }
    if (outConnState != NULL) {
        *outConnState = NULL;
    }
    if (session == NULL || envelope == NULL) {
        return NS_DROP;
    }
    if (envelope->id == NO_WIRE_ID) {
        return NS_FORWARD;
    }
    PendingRequest *slot = findPending(session, envelope->id);
    if (slot == NULL) {
        /* Duplicate or late response from an earlier connection. */
        return NS_DROP;
    }
    PendingRequest request = *slot;
    slot->used = false;

    if (request.internal) {
        if (request.kind == REQUEST_RESUME) {
            json_object *connState = finishResume(session, envelope);
            if (outConnState != NULL) {
                *outConnState = connState;
            } else {
                json_object_put(connState);
            }
        }
        return NS_DROP;
    }
    applyJsResponse(session, request.kind, envelope);
    if (jsId != NULL) {
        *jsId = request.jsId;
    }
    return NS_FORWARD;
}

bool nsHasToken(const NetSession *session) {
    return session != NULL && session->hasToken;
}

const char *nsToken(const NetSession *session) {
    return nsHasToken(session) ? session->token : NULL;
}

uint32_t nsNextBackoffMs(NetSession *session) {
    if (session == NULL) {
        return 0;
    }
    if (session->backoffMs == 0) {
        session->backoffMs = BACKOFF_INITIAL_MS;
    } else if (session->backoffMs < BACKOFF_MAX_MS) {
        session->backoffMs *= 2;
        if (session->backoffMs > BACKOFF_MAX_MS) {
            session->backoffMs = BACKOFF_MAX_MS;
        }
    }
    return session->backoffMs;
}
