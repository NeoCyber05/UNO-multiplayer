#ifndef UNO_SOCIAL_H
#define UNO_SOCIAL_H

#include "server/app.h"
#include "shared/contracts.h"
#include "shared/protocol.h"
#include <stdbool.h>

/* Spec: presence is derived from the connection and activity, never from
 * anything the client claims. */
typedef enum { PRESENCE_OFFLINE, PRESENCE_IDLE, PRESENCE_ONLINE, PRESENCE_IN_MATCH } Presence;

/**
 * @brief Derive a user's presence from server state.
 * @param[in] app Server state owned by the poll loop.
 * @param[in] user Account to inspect.
 * @return PRESENCE_OFFLINE without a connected session; PRESENCE_IN_MATCH while
 *         matchesContains holds a seat; PRESENCE_ONLINE in a room, the queue or a
 *         party; PRESENCE_IDLE otherwise.
 */
Presence socialPresenceOf(App *app, UserId user);

/**
 * @brief Return the wire key of a presence value.
 * @param[in] presence Presence value.
 * @return Static "offline", "idle", "online" or "in_match"; "offline" for an
 *         out-of-range value.
 */
const char *socialPresenceKey(Presence presence);

/**
 * @brief Parse a public friend tag of the exact form "UNO#<id>".
 *
 * Only the canonical text accountsFormatTag produces is accepted: no
 * whitespace, sign, leading zero, zero id or value above INT64_MAX (SQLite ids
 * are signed 64-bit).
 * @param[in] tag Text from the client.
 * @param[out] outUserId Receives the id on success; untouched on failure.
 * @return true when tag is valid, false otherwise (including NULL arguments).
 */
bool socialParseTag(const char *tag, UserId *outUserId);

/**
 * @brief Handle FRIEND_LIST, FRIEND_REQUEST, FRIEND_REPLY, PROFILE_GET,
 *        PROFILE_UPDATE, LEADERBOARD_GET and LOBBY_GET for the session's account.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] conn Connection that sent the request.
 * @param[in] envelope Request envelope borrowed for the call.
 * @return OK once a reply was sent (errors travel as ERROR replies:
 *         UNAUTHENTICATED, BAD_REQUEST, FORBIDDEN, NOT_FOUND, INVALID_ACTION,
 *         STORAGE_ERROR), BAD_REQUEST if the arguments are NULL.
 */
Status socialHandle(App *app, ConnId conn, const Envelope *envelope);

/**
 * @brief Push a fresh FRIEND_STATE to a user and to each accepted friend online.
 *
 * Call after anything that changes the user's presence or public identity.
 * Offline recipients simply miss the push; their next LOGIN sends a new list.
 * @param[in,out] app Server state owned by the poll loop.
 * @param[in] user Account whose presence or identity changed.
 */
void socialPresence(App *app, UserId user);

#endif
