#include "desktop/net_session.h"
#include "shared/protocol.h"
#include <assert.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const char *TEST_TOKEN = "tok-abc-123";

/* Build an incoming envelope; the caller releases payload with json_object_put. */
static Envelope makeEnvelope(int32_t wireId, const char *type, json_object *payload) {
    Envelope envelope = {1, wireId, type, payload};
    return envelope;
}

static const char *conn_state_status(json_object *connState) {
    json_object *status = NULL;
    assert(json_object_object_get_ex(connState, "status", &status));
    return json_object_get_string(status);
}

/* Drive a LOGIN round trip so the session holds TEST_TOKEN. */
static void loginWithToken(NetSession *session) {
    int32_t wireId = 0;
    assert(nsOutgoing(session, 3, "LOGIN", &wireId) == OK);

    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "token", json_object_new_string(TEST_TOKEN));
    json_object_object_add(payload, "me", json_object_new_object());
    Envelope response = makeEnvelope(wireId, "OK", payload);

    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 3);
    assert(connState == NULL);
    json_object_put(payload);
}

static void testIdMapping(void) {
    NetSession *session = nsCreate();
    assert(session != NULL);
    nsOnConnected(session);
    assert(nsState(session) == NS_CONNECTED);

    int32_t wireId = 0;
    assert(nsOutgoing(session, 7, "PING", &wireId) == OK);
    assert(wireId == 1);

    Envelope response = makeEnvelope(1, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 7);
    assert(connState == NULL);

    /* The mapping is consumed by the first response, so a repeat is unknown. */
    assert(nsIncoming(session, &response, &jsId, &connState) == NS_DROP);
    assert(connState == NULL);

    nsDestroy(session);
}

static void testPushForwardedWithZeroJsId(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);

    Envelope push = makeEnvelope(0, "ROOM_UPDATE", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &push, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 0);
    assert(connState == NULL);

    nsDestroy(session);
}

static void testLoginTokenCapturedNotForwardedLogic(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);
    assert(!nsHasToken(session));

    loginWithToken(session);
    assert(nsHasToken(session));
    assert(strcmp(nsToken(session), TEST_TOKEN) == 0);

    nsDestroy(session);
}

static void testLogoutClearsToken(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);
    loginWithToken(session);

    int32_t wireId = 0;
    assert(nsOutgoing(session, 4, "LOGOUT", &wireId) == OK);
    Envelope response = makeEnvelope(wireId, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 4);
    assert(!nsHasToken(session));

    nsDestroy(session);
}

static void testLogoutWipesTokenBytes(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);
    loginWithToken(session);

    /* The token lives in session-owned storage that stays valid until
     * nsDestroy, so its bytes can be inspected after LOGOUT. */
    const char *stored = nsToken(session);
    assert(stored != NULL);
    size_t tokenLength = strlen(TEST_TOKEN);

    int32_t wireId = 0;
    assert(nsOutgoing(session, 4, "LOGOUT", &wireId) == OK);
    Envelope response = makeEnvelope(wireId, "OK", NULL);
    int32_t jsId = -1;
    assert(nsIncoming(session, &response, &jsId, NULL) == NS_FORWARD);
    assert(!nsHasToken(session));
    assert(nsToken(session) == NULL);

    /* No copy of the secret may linger in memory after it is dropped. */
    for (size_t index = 0; index < tokenLength; index++) {
        assert(stored[index] == '\0');
    }

    nsDestroy(session);
}

static void testNonPositiveJsIdRejected(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);

    /* id 0 marks server pushes; a JS request with it would be ambiguous. */
    int32_t wireId = 0;
    assert(nsOutgoing(session, 0, "PING", &wireId) == BAD_REQUEST);
    assert(nsOutgoing(session, -5, "PING", &wireId) == BAD_REQUEST);
    assert(nsOutgoing(session, 1, "PING", &wireId) == OK);
    assert(wireId == 1);

    nsDestroy(session);
}

static void testJsResumeRejected(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);

    int32_t wireId = 0;
    assert(nsOutgoing(session, 1, "RESUME", &wireId) == BAD_REQUEST);

    nsDestroy(session);
}

static void testSendWhileDisconnectedRejects(void) {
    NetSession *session = nsCreate();
    int32_t wireId = 0;
    assert(nsOutgoing(session, 1, "PING", &wireId) == BUSY);

    nsOnConnected(session);
    assert(nsOutgoing(session, 1, "PING", &wireId) == OK);

    nsOnDisconnected(session);
    assert(nsState(session) == NS_DISCONNECTED);
    assert(nsOutgoing(session, 2, "PING", &wireId) == BUSY);

    nsDestroy(session);
}

static void testPendingTableFull(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);

    int32_t wireId = 0;
    for (int32_t jsId = 1; jsId <= NS_MAX_PENDING; jsId++) {
        assert(nsOutgoing(session, jsId, "PING", &wireId) == OK);
    }
    assert(nsOutgoing(session, NS_MAX_PENDING + 1, "PING", &wireId) == BUSY);
    assert(nsInternal(session, "PING") == 0);

    /* Answering one request frees its slot for the next one. */
    Envelope response = makeEnvelope(1, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(nsOutgoing(session, NS_MAX_PENDING + 1, "PING", &wireId) == OK);

    nsDestroy(session);
}

static void testResumeFlow(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);
    loginWithToken(session);
    nsOnDisconnected(session);
    assert(nsHasToken(session));

    /* Resume succeeds: state becomes CONNECTED and the host gets `me`. */
    nsOnConnected(session);
    assert(nsState(session) == NS_RESUMING);
    int32_t wireId = 0;
    assert(nsOutgoing(session, 9, "PING", &wireId) == BUSY);

    int32_t resumeId = nsInternal(session, "RESUME");
    assert(resumeId > 0);
    json_object *payload = json_object_new_object();
    json_object *me = json_object_new_object();
    json_object_object_add(me, "name", json_object_new_string("son"));
    json_object_object_add(payload, "me", me);
    Envelope okResponse = makeEnvelope(resumeId, "OK", payload);

    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &okResponse, &jsId, &connState) == NS_DROP);
    json_object_put(payload);
    assert(connState != NULL);
    assert(strcmp(conn_state_status(connState), "connected") == 0);
    json_object *stateMe = NULL;
    assert(json_object_object_get_ex(connState, "me", &stateMe));
    json_object *name = NULL;
    assert(json_object_object_get_ex(stateMe, "name", &name));
    assert(strcmp(json_object_get_string(name), "son") == 0);
    json_object_put(connState);
    assert(nsState(session) == NS_CONNECTED);
    assert(nsHasToken(session));

    /* Resume fails: token is dropped and the host is told the session expired. */
    nsOnDisconnected(session);
    nsOnConnected(session);
    assert(nsState(session) == NS_RESUMING);
    resumeId = nsInternal(session, "RESUME");
    assert(resumeId > 0);
    json_object *errorPayload = json_object_new_object();
    json_object_object_add(errorPayload, "code", json_object_new_string("UNAUTHENTICATED"));
    json_object_object_add(errorPayload, "message", json_object_new_string("expired"));
    Envelope errorResponse = makeEnvelope(resumeId, "ERROR", errorPayload);

    connState = NULL;
    assert(nsIncoming(session, &errorResponse, &jsId, &connState) == NS_DROP);
    json_object_put(errorPayload);
    assert(connState != NULL);
    assert(strcmp(conn_state_status(connState), "connected") == 0);
    json_object *expired = NULL;
    assert(json_object_object_get_ex(connState, "sessionExpired", &expired));
    assert(json_object_get_boolean(expired));
    json_object_put(connState);
    assert(!nsHasToken(session));
    assert(nsState(session) == NS_CONNECTED);

    nsDestroy(session);
}

static void testInternalPingDropped(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);

    int32_t pingId = nsInternal(session, "PING");
    assert(pingId == 1);
    Envelope response = makeEnvelope(pingId, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &response, &jsId, &connState) == NS_DROP);
    assert(connState == NULL);

    nsDestroy(session);
}

static void testBackoffSequence(void) {
    NetSession *session = nsCreate();
    assert(nsNextBackoffMs(session) == 1000);
    assert(nsNextBackoffMs(session) == 2000);
    assert(nsNextBackoffMs(session) == 4000);
    assert(nsNextBackoffMs(session) == 8000);
    assert(nsNextBackoffMs(session) == 8000);

    nsOnConnected(session);
    assert(nsNextBackoffMs(session) == 1000);

    nsDestroy(session);
}

static void testBackoffKeptUntilResumed(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);
    loginWithToken(session);
    nsOnDisconnected(session);
    assert(nsNextBackoffMs(session) == 1000);
    assert(nsNextBackoffMs(session) == 2000);

    /* A server that accepts and then closes before answering RESUME must
     * not reset the backoff, or it would be retried every second. */
    nsOnConnected(session);
    assert(nsState(session) == NS_RESUMING);
    nsOnDisconnected(session);
    assert(nsNextBackoffMs(session) == 4000);

    /* Once RESUME is answered the session is usable again: start over. */
    nsOnConnected(session);
    int32_t resumeId = nsInternal(session, "RESUME");
    assert(resumeId > 0);
    Envelope okResponse = makeEnvelope(resumeId, "OK", NULL);
    json_object *connState = NULL;
    assert(nsIncoming(session, &okResponse, NULL, &connState) == NS_DROP);
    json_object_put(connState);
    assert(nsState(session) == NS_CONNECTED);
    nsOnDisconnected(session);
    assert(nsNextBackoffMs(session) == 1000);

    nsDestroy(session);
}

static void testWireIdsResetPerConnection(void) {
    NetSession *session = nsCreate();
    nsOnConnected(session);

    int32_t wireId = 0;
    assert(nsOutgoing(session, 1, "PING", &wireId) == OK);
    assert(wireId == 1);
    assert(nsOutgoing(session, 2, "PING", &wireId) == OK);
    assert(wireId == 2);

    nsOnDisconnected(session);
    nsOnConnected(session);
    assert(nsOutgoing(session, 3, "PING", &wireId) == OK);
    assert(wireId == 1);

    /* A response for the previous connection's id 2 must not map to js 2. */
    Envelope stale = makeEnvelope(2, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(nsIncoming(session, &stale, &jsId, &connState) == NS_DROP);

    nsDestroy(session);
}

int main(void) {
    testIdMapping();
    testPushForwardedWithZeroJsId();
    testLoginTokenCapturedNotForwardedLogic();
    testLogoutClearsToken();
    testLogoutWipesTokenBytes();
    testNonPositiveJsIdRejected();
    testJsResumeRejected();
    testSendWhileDisconnectedRejects();
    testPendingTableFull();
    testResumeFlow();
    testInternalPingDropped();
    testBackoffSequence();
    testBackoffKeptUntilResumed();
    testWireIdsResetPerConnection();
    nsDestroy(NULL);
    puts("net_session: OK");
    return 0;
}
