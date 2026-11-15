# Review nhánh `feat/tcp-listener` (Sơn)

- **Ngày review:** 2026-11-15
- **Commit:** `c447580` "update task2,spike12, 3,4,5" (1 commit trên `2ff6cd0`, 34 file, khoảng 7.100 dòng thêm)
- **Đối chiếu với:** [plan](../superpowers/plans/2026-11-11-ubuntu-tcp-uno.md), [spec](../superpowers/specs/2026-11-11-ubuntu-tcp-design.md)
- **Tải giả định:** demo LAN, tối đa 64 kết nối và 16 phòng (theo plan)
- **Chưa kiểm tra:** chưa build hay chạy test (máy review là Windows, không có Ubuntu/GTK). Chưa đọc plan `2026-11-15-tcp-server-network-storage.md` và chưa đọc từng dòng của test C.

## Nhánh này làm gì

Nhánh thêm:

- Server TCP một thread, dùng `poll()` + nonblocking socket, tối đa 64 kết nối.
- SQLite store có transaction cho trận đấu.
- Đăng ký/đăng nhập, hash mật khẩu ở worker thread riêng.
- Phòng, lời mời và party.
- Bản thử desktop GTK/WebKit (spike Task 12): React load qua `uno://app`, bridge JS ↔ C ↔ TCP.

Plan đã tick xong Task 2, 3, 4, 5.

## Kiểm tra có dùng WebSocket không

**Kết quả: không có.** Code mạng đúng yêu cầu TCP thuần, app đúng là desktop app.

- Đã grep toàn bộ `.c/.h/.py/.js/.jsx/.json/.html/Makefile` với các từ khóa: `websocket`, `ws://`, `wss://`, `new WebSocket`, `socket.io`, `Sec-WebSocket`, `Upgrade:`, `libwebsockets`, `fetch(`, `XMLHttpRequest`, `http.server`, `express`, `EventSource`. Không có kết quả nào.
- Server (`server/app.c:168`) dùng `socket(AF_INET, SOCK_STREAM)` → `bind` → `listen` → `accept` → `poll`. Đây là TCP thuần.
- Client native (`desktop/tcp_client.c:39`) dùng `getaddrinfo` → `socket(SOCK_STREAM)` → `connect` nonblocking → kiểm tra `SO_ERROR`. Cũng là TCP thuần.
- Wire format: 4 byte độ dài big-endian + JSON (`shared/protocol.c`), không có HTTP handshake.
- Integration test (`tests/integration/helpers.py:90`) dùng `socket.AF_INET, SOCK_STREAM` thật.
- React **không tự mở kết nối mạng**. JS gửi lệnh qua `window.webkit.messageHandlers.uno` (bridge nội bộ WebKitGTK) sang C, rồi C gửi qua TCP. Asset được load qua scheme `uno://app` từ đĩa, không có HTTP server.
- `client/package.json` không có thư viện `ws` hay `socket.io`. `client/src/net/` chưa tồn tại (Task 13).
- Trong tài liệu, WebSocket chỉ xuất hiện ở câu "không WebSocket" của spec và plan.

**Lưu ý (không thuộc nhánh này):** Trong thư mục làm việc của Bách có 2 file chưa commit còn mô tả hướng WebSocket cũ: `ARCHITECTURE.md` (22 chỗ) và `FLOW.md` (2 chỗ). Task 15 đã ghi phải viết lại `ARCHITECTURE.md`. Đừng commit nhầm hai file này kèm sơ đồ WS cũ, kẻo người chấm hiểu sai kiến trúc.

---

## Phải sửa (trước khi merge)

### 1. Makefile bắt mọi thứ phải có GTK/WebKit
`Makefile:4-8,33`

- **Là gì:** `GTK_CFLAGS`/`GTK_LIBS` được thêm vào `CPPFLAGS`/`LDLIBS` chung. Mọi test còn link thêm `desktop/*.c`.
- **Vấn đề:** `uno-server` link cả webkit2gtk. Muốn chạy `make test NAME=game` cũng phải cài `libwebkit2gtk-4.1-dev`. Máy WSL chỉ có json-c (như Task 1 yêu cầu) thì không build được nữa.
- **Sửa:** Chỉ thêm cờ GTK cho `$(BUILD)/uno-desktop` và `test_bridge`. Bỏ `$(desktop_modules)` khỏi rule test chung.
- **Nếu bỏ qua:** Server và test engine không build được trên máy không có GUI.

### 2. Peer chậm bị đánh dấu nhưng không bao giờ bị đóng
`server/app.c:361-387`, `shared/net.c:110-112`

- **Là gì:** Khi buffer gửi quá 256KB, `conn_queue` đặt `closing = true`.
- **Vấn đề:** Server chỉ kiểm tra `closing` khi có `POLLIN`/`POLLOUT`. Client treo không đọc mà cũng không gửi (ví dụ app bị `kill -STOP`) thì không bao giờ có hai sự kiện đó. Slot bị giữ mãi, mọi tin gửi tới nó bị bỏ âm thầm. Plan yêu cầu "đóng riêng peer chậm" (Review Focus 1).
- **Sửa:** Cuối mỗi vòng `poll`, thêm `if (slot.active && slot.conn.closing) app_close_slot(...)`. Thêm integration test: client không đọc, nhận hơn 256KB, phải bị server đóng.
- **Nếu bỏ qua:** Mỗi client treo chiếm vĩnh viễn 1/64 slot.

### 3. App desktop tự thoát sau 10 giây
`desktop/main.c:448,503`

- **Là gì:** `timeout_sec = 10` mặc định, hết giờ thì gọi `exit(1)`.
- **Vấn đề:** Timeout này dành cho smoke test nhưng đang là mặc định của app thật. Người chơi bị đá ra sau 10 giây.
- **Sửa:** Đổi mặc định thành `0`. Riêng `desktop-smoke` truyền `--timeout 5`.
- **Nếu bỏ qua:** Không chơi được ván nào.

### 4. Whitelist của bridge dùng tên lệnh khác server
`desktop/bridge.c:37-62`

- **Là gì:** Danh sách lệnh JS được phép gửi.
- **Vấn đề:** Bridge cho `AUTH_LOGIN`, `AUTH_REGISTER`, `INVITE_RESPOND`, nhưng server và spec dùng `LOGIN`, `REGISTER`, `INVITE_REPLY`. Bridge còn thiếu `ROOM_MODE`, `LOGOUT`, và có lệnh spec không có (`AUTH_GUEST`, `GAME_ACTION`, `MATCH_CONFIRM`...).
- **Sửa:** Copy đúng danh sách opcode trong spec §3 / `docs/protocol.md`. Bỏ `SPIKE_DONE` khỏi bản chính thức.
- **Nếu bỏ qua:** UI không đăng nhập được, Task 13/14 bị chặn.

### 5. Vào phòng mới thì tự rời phòng cũ, kể cả phòng đang trong trận
`server/rooms.c:190`, `:240`, accept invite ở `:973`

- **Là gì:** `rooms_create`, `rooms_join` và việc chấp nhận lời mời đều gọi `rooms_leave` lên phòng hiện tại trước.
- **Vấn đề:** Người đang ở phòng `IN_MATCH` chỉ cần chấp nhận một lời mời là bị xóa khỏi ghế của trận đang chạy. Phòng cũ cũng không nhận `ROOM_STATE` mới. Plan ghi "user busy bị từ chối" và "không cùng lúc hai phòng".
- **Sửa:** Nếu `rooms_find_by_user` đã thấy phòng thì trả `BUSY`, không tự rời. Thêm test.
- **Nếu bỏ qua:** Khi nối Task 9, ghế trong trận bị xóa giữa ván.

---

## Nên sửa (trước Task 9)

### 6. ROOM_START không có trận thật, và có thể bấm start hai lần
`server/rooms.c:372-409`, `server/app.c:142`

- **Vấn đề:** `app_init` gọi `rooms_init(..., NULL)`, nên start vẫn trả OK và đặt phòng `IN_MATCH` dù không có trận nào. Khi đó `rooms_disconnect` bỏ qua phòng này, nên người rớt mạng nằm lại mãi (phòng zombie). Ngoài ra không có kiểm tra `status == WAITING`, nên host bấm 2 lần sẽ gọi `on_start` 2 lần.
- **Sửa:** `if (room->status != ROOM_STATUS_WAITING) return INVALID_ACTION;` và `if (!r->on_start) return BUSY;`.

### 7. Token không bao giờ hết hạn
`server/accounts.c:299-321`

- **Vấn đề:** Spec §3 ghi "hết phiên sau 24h". Code có lưu `created_at_ms` nhưng không kiểm tra, nên token lộ ra dùng được mãi. Plan có tick test "token expired" nhưng không có test này.
- **Sửa:** Trong `sessions_resume`, nếu `now - created_at_ms > 24h` thì xóa entry và trả `UNAUTHENTICATED`. Thêm unit test.

### 8. Mọi lỗi DB khi đăng ký đều báo "email đã tồn tại"
`server/accounts.c:619`

- **Vấn đề:** `store_account_create` phân biệt được trùng email (`BAD_REQUEST`) với lỗi khác (`STORAGE_ERROR`), nhưng `auth_drain` gộp cả hai. Ví dụ đĩa đầy thì người dùng bị báo trùng email.
- **Sửa:** Rẽ nhánh theo status và trả `STORAGE_ERROR` cho trường hợp DB lỗi.

### 9. Lời mời hết hạn không bao giờ được giải phóng
`server/rooms.c:883-892`

- **Vấn đề:** Lời mời chỉ bị tắt khi người nhận trả lời. Sau 64 lời mời bị bỏ qua, mọi lời mời mới đều ghi đè slot 0 và giết lời mời đang chờ trước đó.
- **Sửa:** Khi tìm slot trống, coi lời mời quá 30 giây là trống.

### 10. Party: một người vào được hai party, và state không được đẩy đi
`server/rooms.c:1004-1040`, `:575-584`

- **Vấn đề:** Khi chấp nhận lời mời party, code không kiểm tra lại `party_find_by_user(user_id)`. A mời C, B mời C, C chấp nhận cả hai thì C ở trong 2 party. `PARTY_LEAVE`, disconnect và tự rời phòng đều không gửi `PARTY_STATE`/`ROOM_STATE` cho người còn lại.
- **Sửa:** Kiểm tra lại lúc chấp nhận (trả `BUSY`). Broadcast trước khi xóa party.

### 11. ProfileDelta cộng dồn streak/bestStreak/peakElo
`server/store.c:445-447`

- **Vấn đề:** SQL làm `progress = progress + excluded.progress` cho mọi key. Chuỗi thắng phải về 0 khi thua, nhưng delta là `uint32` nên không biểu diễn được. `peakElo` sau 2 trận thành 1516+1532.
- **Đã chốt (Bách, 15/11):** tách rõ hai loại dữ liệu. *Bộ đếm* thì cộng dồn. *Trạng thái* thì Bách tính giá trị mới và Sơn ghi đè. Tiến độ badge không lưu riêng mà suy ra từ số liệu.

  **1. `shared/contracts.h`, thay `ProfileDelta`:**
  ```c
  typedef enum {
      BADGE_FIRST_WIN  = 1u << 0, /* wins >= 1 */
      BADGE_WIN_100    = 1u << 1, /* wins >= 100 */
      BADGE_STREAK_10  = 1u << 2, /* bestStreak >= 10 */
      BADGE_PLUS4      = 1u << 3, /* plus4 >= 50 */
      BADGE_UNO_MASTER = 1u << 4, /* unoCalls >= 300 */
      BADGE_TEAM       = 1u << 5, /* teamWins >= 50 */
      BADGE_MATCHES    = 1u << 6, /* matches >= 500 */
      BADGE_DIAMOND    = 1u << 7  /* peakElo >= 1800 */
  } BadgeBit;

  typedef struct {
      uint32_t matches, wins, teamWins, plus4, unoCalls;
      int32_t  streak;      /* >0 chuỗi thắng, <0 chuỗi thua */
      uint32_t bestStreak;
      int32_t  peakElo;
  } ProfileStats;           /* hồ sơ hiện tại; Store đọc ra, profile_compute dùng */

  typedef struct {
      struct { uint32_t matches, wins, teamWins, plus4, unoCalls; } add; /* cộng dồn */
      int32_t  streak;      /* giá trị MỚI, ghi đè */
      uint32_t bestStreak;  /* giá trị MỚI, ghi đè */
      int32_t  peakElo;     /* giá trị MỚI, ghi đè */
      uint32_t unlocked;    /* BadgeBit mở lần đầu trong trận này */
  } ProfileDelta;
  ```

  **2. Schema** (version 1 chưa có dữ liệu thật nên đổi thẳng, không cần migration):
  - Bỏ bảng `achievements(key, progress, ...)` dạng key-value.
  - Thêm bảng `profile_stats`, mỗi user một dòng:
    ```sql
    profile_stats(
        user_id      INTEGER PRIMARY KEY REFERENCES accounts(id),
        matches      INTEGER NOT NULL DEFAULT 0,
        wins         INTEGER NOT NULL DEFAULT 0,
        team_wins    INTEGER NOT NULL DEFAULT 0,
        plus4        INTEGER NOT NULL DEFAULT 0,
        uno_calls    INTEGER NOT NULL DEFAULT 0,
        streak       INTEGER NOT NULL DEFAULT 0,
        best_streak  INTEGER NOT NULL DEFAULT 0,
        peak_elo     INTEGER NOT NULL DEFAULT 1500
    )
    ```
  - Thêm bảng ghi lần mở badge:
    ```sql
    achievements(
        user_id     INTEGER NOT NULL,
        badge       INTEGER NOT NULL,
        unlocked_at INTEGER NOT NULL,
        match_id    TEXT NOT NULL,
        PRIMARY KEY (user_id, badge)
    )
    ```

  **3. `store_finalize`**, trong cùng transaction đã có:
  - Với mỗi user:
    ```sql
    UPDATE profile_stats
       SET matches = matches + ?, ...,
           streak = ?, best_streak = ?, peak_elo = ?
     WHERE user_id = ?
    ```
  - Với mỗi bit trong `unlocked`: `INSERT OR IGNORE INTO achievements ...`.
  - Thêm `Status store_profile_stats(Store *, UserId, ProfileStats *out)`.
  - Dòng `profile_stats` được tạo cùng `store_account_create`.

  **4. `profile_compute`** (Bách) đọc `ProfileStats` hiện tại, rồi tính:
  - `streak`: thắng thì `max(s,0)+1`, thua thì `min(s,0)-1`.
  - `bestStreak = max(cũ, streak)`.
  - `peakElo = max(cũ, eloAfter)`.
  - `unlocked`: những bit qua ngưỡng ở số liệu mới mà chưa qua ở số liệu cũ.

  Trận custom hoặc aborted (không xếp hạng ELO) vẫn cộng `matches`/`wins`. Bách chốt điểm này khi làm Task 11.

  **Vì sao chọn cách này:**
  - Mỗi trường chỉ có một nghĩa, không còn chuyện "cộng hay ghi đè".
  - Chống ghi lặp đã có sẵn: `store_finalize` thấy trạng thái `FINALIZED` thì không ghi lại.
  - Server một thread nên đọc rồi ghi không bị tranh chấp.
  - Tiến độ badge (ví dụ 37/100 trận thắng) luôn khớp với số liệu, vì suy ra trực tiếp chứ không lưu thêm một bản dễ lệch.
  - Luật nghiệp vụ nằm ở module của Bách; Sơn chỉ lưu đúng những gì được giao.

  **Phải xong trước Task 10.** Ai sửa file nào: Sơn sửa `contracts.h`, `store.c`, `test_store.c`; Bách viết `profile.c` ở Task 11.

### 12. Token lộ cho JS, và trang lạ được gọi bridge
`desktop/bridge.c:155-179`, `desktop/main.c:457-466`

- **Vấn đề:** `bridge_emit` chuyển nguyên OK của LOGIN (có `token`) vào JS. Spec ghi "JS không nhận token". `--uri` hoặc biến môi trường `UNO_URI` cho load URL bất kỳ, và không có handler `decide-policy` để chặn chuyển trang ra ngoài. Trang lạ khi đó vẫn có `messageHandlers.uno` và gửi lệnh lên server được. `bridge_dispatch` cũng không kiểm tra origin.
- **Sửa:** Native giữ token và xóa nó khỏi payload trước khi emit. Bỏ `--uri`/`UNO_URI`. Chặn mọi navigation không phải `uno://`. Đây là phần còn lại của Task 12.

### 13. Chưa có giới hạn 30 request/giây mỗi kết nối
spec §3; Task 4 đã tick

- **Vấn đề:** Một client gửi lệnh liên tục làm vòng `poll` và SQLite bận cho cả server.
- **Sửa:** Token bucket đặt trong `AppConnSlot`, kiểm tra ở `app_on_frame`.

### 14. Plan tick xong nhưng thiếu test đã liệt kê
- **Vấn đề:** Không có test cho:
  - đóng kết nối giữa frame
  - peer chậm qua TCP thật
  - token hết hạn
  - lời mời hết hạn 30 giây
  - `BUSY` khi đang ở phòng khác
  - hai người tranh ghế cuối
  - LOGIN mới ngắt connection cũ

  `test_max_connections` chấp nhận gói `type: "SERVER_FULL"`, trong khi spec yêu cầu `ERROR {code: "SERVER_FULL"}` (`server/app.c:341`).
- **Sửa:** Bỏ tick các checkbox chưa đủ. Thêm các test trên. Gửi `ERROR` đúng spec.

### 15. Desktop không gửi tiếp phần frame còn dở
`desktop/main.c:500`, `desktop/tcp_client.c:88-94`

- **Vấn đề:** Watch GLib chỉ có `G_IO_IN`. Khi `send` trả `EAGAIN`, phần còn lại nằm trong buffer cho tới lần gửi sau.
- **Sửa:** Bật `G_IO_OUT` khi `out_len > out_pos`, rồi gọi flush trong callback.

---

## Có thì tốt

### 16. Code thừa nên xóa (khoảng −170 dòng)
- `server/schema.sql` trùng nguyên `EMBEDDED_SCHEMA_SQL` trong `store.c` mà không ai đọc.
- `tests/fixtures/spike_test.html` giống hệt `index.html`, kèm nhánh fallback ở `bridge.c:301-312`.
- `client_poll` (`tcp_client.c:96`) không dùng.
- `g_last_rooms` (`rooms.c:14`) là biến global chỉ để test.
- Worker tự sinh token thay vì gọi `accounts_generate_token`, nên hàm được test lại không phải hàm đang chạy.
- `store_open` bị gọi 2 lần (`app.c:157` và `:267`). `auth_drain` bị gọi 3 lần mỗi vòng `poll`.
- Mảng tên mode `{"classic","2v2","side","draft"}` lặp 3 lần.

### 17. Hạn lời mời đo bằng `CLOCK_REALTIME`
`rooms.c:19`

Khi đồng hồ máy bị chỉnh lùi, phép trừ không dấu bị tràn và mọi lời mời hết hạn ngay. Nên dùng `CLOCK_MONOTONIC` như `accounts.c`.

### 18. Ép kiểu seat không kiểm tra miền
`rooms.c:739,771`

`(Seat)seat_val` với 257 thành ghế 1. Nên kiểm tra `0..3` trước khi cast.

---

## Bách có phải chờ Sơn sửa xong không

Không. Chỉ cần chờ ở mốc nối code, giống các điểm chờ đã có trong plan.

| Việc của Bách | Có chờ không | Ghi chú |
|---|---|---|
| Task 9, phần `match.c` + `test_match.c` (đồng hồ giả, engine thật) | Không | Không đụng file của Sơn |
| Task 13, phần adapter `view.js` + test với `views.json` | Không | Không cần bridge chạy thật |
| Build/test trên máy không có GTK | Chỉ khi merge nhánh của Sơn | Lỗi 1 (Makefile). Cứ làm trên nhánh tách từ `main`, hoặc cài thư viện GTK/WebKit dev |
| Task 9, nối `matches_start` vào phòng + chạy integration | Có | Cần Sơn sửa 5, 6 (rooms) và nối dispatch, như plan đã ghi |
| Task 13/14, chạy thật trong app desktop | Có | Cần Sơn sửa 3, 4, 12, 15 (desktop/bridge) |
| Task 11, phần hồ sơ | Có | Cần Sơn đổi contract ở mục 11 trước (vốn đã phải chờ Task 10) |

## Kết luận

- **Merge:** sửa 1–5 trước.
- **Trước Task 9:** xong 6–15.
- **Mạng:** đúng TCP thuần, không có WebSocket.
- **Chưa kiểm tra:** build, test và ASan trên Ubuntu thật.
