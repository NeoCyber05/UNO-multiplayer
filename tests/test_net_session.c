#include "desktop/net_session.h"
#include "shared/protocol.h"
#include <assert.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const char *TEST_TOKEN = "tok-abc-123";

/* Build an incoming envelope; the caller releases payload with json_object_put. */
static Envelope make_envelope(int32_t wireId, const char *type, json_object *payload)
{
    Envelope envelope = {1, wireId, type, payload};
    return envelope;
}

static const char *conn_state_status(json_object *connState)
{
    json_object *status = NULL;
    assert(json_object_object_get_ex(connState, "status", &status));
    return json_object_get_string(status);
}

/* Drive a LOGIN round trip so the session holds TEST_TOKEN. */
static void login_with_token(NetSession *session)
{
    int32_t wireId = 0;
    assert(ns_outgoing(session, 3, "LOGIN", &wireId) == OK);

    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "token", json_object_new_string(TEST_TOKEN));
    json_object_object_add(payload, "me", json_object_new_object());
    Envelope response = make_envelope(wireId, "OK", payload);

    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 3);
    assert(connState == NULL);
    json_object_put(payload);
}

static void test_id_mapping(void)
{
    NetSession *session = ns_create();
    assert(session != NULL);
    ns_on_connected(session);
    assert(ns_state(session) == NS_CONNECTED);

    int32_t wireId = 0;
    assert(ns_outgoing(session, 7, "PING", &wireId) == OK);
    assert(wireId == 1);

    Envelope response = make_envelope(1, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 7);
    assert(connState == NULL);

    /* The mapping is consumed by the first response, so a repeat is unknown. */
    assert(ns_incoming(session, &response, &jsId, &connState) == NS_DROP);
    assert(connState == NULL);

    ns_destroy(session);
}

static void test_push_forwarded_with_zero_js_id(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);

    Envelope push = make_envelope(0, "ROOM_UPDATE", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &push, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 0);
    assert(connState == NULL);

    ns_destroy(session);
}

static void test_login_token_captured_not_forwarded_logic(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);
    assert(!ns_has_token(session));

    login_with_token(session);
    assert(ns_has_token(session));
    assert(strcmp(ns_token(session), TEST_TOKEN) == 0);

    ns_destroy(session);
}

static void test_logout_clears_token(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);
    login_with_token(session);

    int32_t wireId = 0;
    assert(ns_outgoing(session, 4, "LOGOUT", &wireId) == OK);
    Envelope response = make_envelope(wireId, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(jsId == 4);
    assert(!ns_has_token(session));

    ns_destroy(session);
}

static void test_logout_wipes_token_bytes(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);
    login_with_token(session);

    /* The token lives in session-owned storage that stays valid until
     * ns_destroy, so its bytes can be inspected after LOGOUT. */
    const char *stored = ns_token(session);
    assert(stored != NULL);
    size_t tokenLength = strlen(TEST_TOKEN);

    int32_t wireId = 0;
    assert(ns_outgoing(session, 4, "LOGOUT", &wireId) == OK);
    Envelope response = make_envelope(wireId, "OK", NULL);
    int32_t jsId = -1;
    assert(ns_incoming(session, &response, &jsId, NULL) == NS_FORWARD);
    assert(!ns_has_token(session));
    assert(ns_token(session) == NULL);

    /* No copy of the secret may linger in memory after it is dropped. */
    for (size_t index = 0; index < tokenLength; index++) {
        assert(stored[index] == '\0');
    }

    ns_destroy(session);
}

static void test_non_positive_js_id_rejected(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);

    /* id 0 marks server pushes; a JS request with it would be ambiguous. */
    int32_t wireId = 0;
    assert(ns_outgoing(session, 0, "PING", &wireId) == BAD_REQUEST);
    assert(ns_outgoing(session, -5, "PING", &wireId) == BAD_REQUEST);
    assert(ns_outgoing(session, 1, "PING", &wireId) == OK);
    assert(wireId == 1);

    ns_destroy(session);
}

static void test_js_resume_rejected(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);

    int32_t wireId = 0;
    assert(ns_outgoing(session, 1, "RESUME", &wireId) == BAD_REQUEST);

    ns_destroy(session);
}

static void test_send_while_disconnected_rejects(void)
{
    NetSession *session = ns_create();
    int32_t wireId = 0;
    assert(ns_outgoing(session, 1, "PING", &wireId) == BUSY);

    ns_on_connected(session);
    assert(ns_outgoing(session, 1, "PING", &wireId) == OK);

    ns_on_disconnected(session);
    assert(ns_state(session) == NS_DISCONNECTED);
    assert(ns_outgoing(session, 2, "PING", &wireId) == BUSY);

    ns_destroy(session);
}

static void test_pending_table_full(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);

    int32_t wireId = 0;
    for (int32_t jsId = 1; jsId <= NS_MAX_PENDING; jsId++) {
        assert(ns_outgoing(session, jsId, "PING", &wireId) == OK);
    }
    assert(ns_outgoing(session, NS_MAX_PENDING + 1, "PING", &wireId) == BUSY);
    assert(ns_internal(session, "PING") == 0);

    /* Answering one request frees its slot for the next one. */
    Envelope response = make_envelope(1, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &response, &jsId, &connState) == NS_FORWARD);
    assert(ns_outgoing(session, NS_MAX_PENDING + 1, "PING", &wireId) == OK);

    ns_destroy(session);
}

static void test_resume_flow(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);
    login_with_token(session);
    ns_on_disconnected(session);
    assert(ns_has_token(session));

    /* Resume succeeds: state becomes CONNECTED and the host gets `me`. */
    ns_on_connected(session);
    assert(ns_state(session) == NS_RESUMING);
    int32_t wireId = 0;
    assert(ns_outgoing(session, 9, "PING", &wireId) == BUSY);

    int32_t resumeId = ns_internal(session, "RESUME");
    assert(resumeId > 0);
    json_object *payload = json_object_new_object();
    json_object *me = json_object_new_object();
    json_object_object_add(me, "name", json_object_new_string("son"));
    json_object_object_add(payload, "me", me);
    Envelope okResponse = make_envelope(resumeId, "OK", payload);

    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &okResponse, &jsId, &connState) == NS_DROP);
    json_object_put(payload);
    assert(connState != NULL);
    assert(strcmp(conn_state_status(connState), "connected") == 0);
    json_object *stateMe = NULL;
    assert(json_object_object_get_ex(connState, "me", &stateMe));
    json_object *name = NULL;
    assert(json_object_object_get_ex(stateMe, "name", &name));
    assert(strcmp(json_object_get_string(name), "son") == 0);
    json_object_put(connState);
    assert(ns_state(session) == NS_CONNECTED);
    assert(ns_has_token(session));

    /* Resume fails: token is dropped and the host is told the session expired. */
    ns_on_disconnected(session);
    ns_on_connected(session);
    assert(ns_state(session) == NS_RESUMING);
    resumeId = ns_internal(session, "RESUME");
    assert(resumeId > 0);
    json_object *errorPayload = json_object_new_object();
    json_object_object_add(errorPayload, "code", json_object_new_string("UNAUTHENTICATED"));
    json_object_object_add(errorPayload, "message", json_object_new_string("expired"));
    Envelope errorResponse = make_envelope(resumeId, "ERROR", errorPayload);

    connState = NULL;
    assert(ns_incoming(session, &errorResponse, &jsId, &connState) == NS_DROP);
    json_object_put(errorPayload);
    assert(connState != NULL);
    assert(strcmp(conn_state_status(connState), "connected") == 0);
    json_object *expired = NULL;
    assert(json_object_object_get_ex(connState, "sessionExpired", &expired));
    assert(json_object_get_boolean(expired));
    json_object_put(connState);
    assert(!ns_has_token(session));
    assert(ns_state(session) == NS_CONNECTED);

    ns_destroy(session);
}

static void test_internal_ping_dropped(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);

    int32_t pingId = ns_internal(session, "PING");
    assert(pingId == 1);
    Envelope response = make_envelope(pingId, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &response, &jsId, &connState) == NS_DROP);
    assert(connState == NULL);

    ns_destroy(session);
}

static void test_backoff_sequence(void)
{
    NetSession *session = ns_create();
    assert(ns_next_backoff_ms(session) == 1000);
    assert(ns_next_backoff_ms(session) == 2000);
    assert(ns_next_backoff_ms(session) == 4000);
    assert(ns_next_backoff_ms(session) == 8000);
    assert(ns_next_backoff_ms(session) == 8000);

    ns_on_connected(session);
    assert(ns_next_backoff_ms(session) == 1000);

    ns_destroy(session);
}

static void test_wire_ids_reset_per_connection(void)
{
    NetSession *session = ns_create();
    ns_on_connected(session);

    int32_t wireId = 0;
    assert(ns_outgoing(session, 1, "PING", &wireId) == OK);
    assert(wireId == 1);
    assert(ns_outgoing(session, 2, "PING", &wireId) == OK);
    assert(wireId == 2);

    ns_on_disconnected(session);
    ns_on_connected(session);
    assert(ns_outgoing(session, 3, "PING", &wireId) == OK);
    assert(wireId == 1);

    /* A response for the previous connection's id 2 must not map to js 2. */
    Envelope stale = make_envelope(2, "OK", NULL);
    int32_t jsId = -1;
    json_object *connState = NULL;
    assert(ns_incoming(session, &stale, &jsId, &connState) == NS_DROP);

    ns_destroy(session);
}

int main(void)
{
    test_id_mapping();
    test_push_forwarded_with_zero_js_id();
    test_login_token_captured_not_forwarded_logic();
    test_logout_clears_token();
    test_logout_wipes_token_bytes();
    test_non_positive_js_id_rejected();
    test_js_resume_rejected();
    test_send_while_disconnected_rejects();
    test_pending_table_full();
    test_resume_flow();
    test_internal_ping_dropped();
    test_backoff_sequence();
    test_wire_ids_reset_per_connection();
    ns_destroy(NULL);
    puts("net_session: OK");
    return 0;
}
