# Bàn giao Sơn → Bách (branch feat/son-task9-12-10-11)

Ghi chú từ Task 10 và đợt sửa sau review toàn branch. Mục 3 đã được thay bằng bản sửa ở cuối.

## Handoff for Bách

1. **`match.c` changed in 3 places in Task 6 (commit `69fd1e0`), plus `#include "rating.h"`:**
   - (1) `matchesStart` now accepts ranked rooms. It rejects any bot seat when `room->ranked`, and stores `result.ranked`, which is passed to `store_match_start`.
   - (2) `beginFinish` calls `rating_apply(&match->result, isBot)` when status is `MATCH_FINALIZED`. A seat counts as a bot only when its userId is 0, so a player who leaves with MATCH_LEAVE is still rated.
   - (3) `storeAbort` now calls `store_abort(...)` instead of running raw SQL against `store_get_db`.
2. **Where to hook `profileCompute`:** in `finishMatch` (server/match.c, around line 288), just before `store_finalize(matches->store, &match->result, NULL)`.
   - The third argument is `NULL` today, and the comment says "NULL preserves the existing profile streak and peak until Task 11".
   - Call `profileCompute(matches->store, &match->result, deltas)` and pass `deltas` instead of `NULL`.
   - On error, go through the same `pauseMatch` retry path.
3. **The native side emits `CONNECTION_STATE` with `status:"reconnecting"`, and `sessionState.js` must handle it.**
   - The reducer only special-cases `'disconnected'`, which native never sends. Only SERVER_SHUTDOWN sets it.
   - So `reconnecting` currently just stores the string. The UI needs a reconnect banner, and should block actions or show that they are disabled.
   - On `connected`, the payload may carry `me` from RESUME, but the reducer ignores `payload.me`.
   - `sessionExpired` is already handled in `session.jsx` (AUTH_CLEAR).
   - After a successful RESUME the server resends ROOM_STATE, PARTY_STATE, QUEUE_STATE, FRIEND_STATE and GAME_STATE. Keeping the room, party and queue during `reconnecting` is therefore fine.
4. **RESUME no longer returns `token`.** Only native stores the token (from LOGIN OK). The bridge strips `token` from every payload sent to JS, so JS never sees it, including on LOGIN OK.

Found while verifying (client-side, for Bách):

5. **PARTY_STATE dissolve** arrives as `{partyId, leaderId:"0", members:[], revision+1}`. The reducer stores it as a live party, so the UI should treat `members.length === 0` as "no party".
6. **QUEUE_STATE `active:false`** (left the queue, or matched with `matchId`) is stored as `state.queue`. The UI must check `active` and not just whether `queue` is set.
7. **A kicked user** gets a ROOM_STATE in which they have no seat. The client should drop the room when `me.userId` is not in `seats`.
8. **No room push after a match.** After a room match the server frees the room silently. The reducer already clears `room` on MATCH_RESULT, so this works; don't wait for a ROOM_STATE.
9. **Unemitted push types.** The server never emits LOBBY_STATE or LEADERBOARD_STATE. `stateEvents` lists them, but the data only comes from LOBBY_GET, LEADERBOARD_GET and FRIEND_STATE.


## Handoff for Bách (replaces item 3 of task-10-report.md)

3. **What the client must do with `CONNECTION_STATE`:**
   - **(a) `me`:** nothing to do. `session.jsx:17` already maps `CONNECTION_STATE.me` to `AUTH_OK`, and `sessionExpired` is mapped to `AUTH_CLEAR` at line 18.
   - **(b) `disconnected`:** native never sends this status. JS sets it in two places, and only one of them resets anything:
     - SERVER_SHUTDOWN (`sessionState.js:25`) sets `connection: 'disconnected'` only. It does not reset room, party or queue.
     - The initial HELLO failure (`session.jsx:26`) sends a local CONNECTION_STATE `disconnected`, and that branch is the one that resets room, queue, party and invites.
   - **(c) `reconnecting`:** `sessionReducer` only special-cases `disconnected`, so it must also reset room, party, queue and game on `reconnecting`. The reason:
     - When the server sees the drop (EOF/reset, or 15 s without a frame), it dissolves the party, removes the user from a waiting room and drops the queue entry. The match may also end or be aborted while the user is offline.
     - RESUME then resends only what still exists (GAME_STATE, ROOM_STATE, PARTY_STATE, QUEUE_STATE, FRIEND_STATE). There is no explicit "none" snapshot, so keeping the old state would show a room, party or queue that no longer exists.
     - The UI still needs a reconnect banner and should disable actions while `reconnecting`. Requests sent then are answered at once with `ERROR {code:"DISCONNECTED", message}`.


## Kiểm tra thủ công còn thiếu

- Chưa chạy: 4 desktop trên LAN thật, rút Wi-Fi rồi nối lại, đóng cửa sổ bằng window manager khi đang reconnect (Task 4 Step 5).
