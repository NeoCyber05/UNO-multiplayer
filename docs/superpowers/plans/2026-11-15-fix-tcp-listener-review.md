# Fix TCP Listener Review Findings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Giải quyết toàn diện 18 điểm review (1–5 bắt buộc trước merge, 6–15 trước Task 9, 16–18 cleanups) trên nhánh `feat/tcp-listener` để củng cố độ bền, bảo mật và tính đúng đắn của toàn bộ hạ tầng mạng, lưu trữ và sảnh.

**Architecture:** Tách biệt build GUI (GTK3/WebKitGTK) khỏi server và core unit test suite; đóng kết nối slow peer và áp dụng token bucket rate limiting (30 req/s, burst 60); sửa triệt để vòng đời phòng/party/invite (trả `BUSY` khi đang bận, kiểm tra `status == WAITING` và `on_start != NULL`, thu hồi invite 30s); bảo mật desktop bridge (chặn navigation ngoài `uno://`, giấu token khỏi Webview, sửa opcode whitelist); thiết kế lại `ProfileDelta` và schema `profile_stats`/`achievements` theo đúng thỏa thuận Bách–Sơn 15/11.

**Tech Stack:** C11 (GCC), GNU Make, POSIX Sockets, `poll()`, `pthreads`, `json-c`, `libsqlite3`, `libsodium`, GTK3 + WebKitGTK 4.1 (`webkit2gtk-4.1`), Python 3 (`unittest`).

**Spec:** [docs/superpowers/specs/2026-11-11-ubuntu-tcp-design.md](../specs/2026-11-11-ubuntu-tcp-design.md), [docs/superpowers/plans/2026-11-11-ubuntu-tcp-uno.md](2026-11-11-ubuntu-tcp-uno.md), và [docs/2026-11-15-feat-tcp-listener-review.md](../../2026-11-15-feat-tcp-listener-review.md).

## Global Constraints

- Chỉ chạy Ubuntu; server và client mạng viết bằng C dùng POSIX TCP socket; không WebSocket.
- Tách biệt rõ ràng build: `uno-server` và unit test không liên quan GUI không được link `gtk+-3.0` hay `webkit2gtk-4.1`.
- Mọi opcode client gửi lên phải khớp 100% danh mục spec §3 (`LOGIN`, `REGISTER`, `RESUME`, `LOGOUT`, `ROOM_CREATE`, `ROOM_JOIN`, `ROOM_LEAVE`, `ROOM_BOT`, `ROOM_KICK`, `ROOM_MODE`, `ROOM_START`, `INVITE_SEND`, `INVITE_REPLY`, `PARTY_LEAVE`, `QUEUE_JOIN`, `QUEUE_LEAVE`, `PLAY`, `DRAW`, `PASS`, `SAY_UNO`, `DRAFT_PICK`, `MATCH_LEAVE`, `CHAT`, `PING`, `HELLO`).
- Bảo mật token: Native C lưu giữ session token và tự động loại bỏ trường `token` khỏi payload trước khi emit `OK` sang JavaScript Webview; chặn mọi navigation không bắt đầu bằng `uno://`.
- Giới hạn hệ thống: Rate limit 30 req/s (burst 60) mỗi connection; buffer output tối đa 256 KiB (`262144` bytes), đóng ngay client chậm khi vượt quá ở cuối vòng poll; từ chối client thứ 65 bằng format chuẩn `Envelope { v: 1, id: 0, type: "ERROR", payload: { code: "SERVER_FULL", message: "..." } }`.
- Phòng và Party: Người đang ở phòng (kể cả `WAITING` hay `IN_MATCH`) bị từ chối `BUSY` khi cố tạo hoặc vào phòng khác; `ROOM_START` chỉ chạy khi `status == ROOM_STATUS_WAITING` và `on_start != NULL`; invite quá 30s được thu hồi; người chơi không thể ở trong 2 party cùng lúc; party giải phóng phải phát `PARTY_STATE` cho thành viên còn lại.
- Dữ liệu hồ sơ: `ProfileDelta` tách biệt phần cộng dồn (`add`) và phần ghi đè (`streak`, `bestStreak`, `peakElo`, `unlocked`); schema gồm bảng `profile_stats` (1 dòng/user) và `achievements` (`user_id, badge, unlocked_at, match_id`).

## Review Focus

1. **Slow Peer Real Disconnection:** Client nhận dữ liệu dồn quá 256 KiB mà không đọc phải bị server ngắt kết nối ngay ở cuối vòng `poll`, giải phóng slot fd mà không làm nghẽn 63 client còn lại (Review Item 2).
2. **State Concurrency & Room/Party Invariants:** Người chơi đang ở phòng/trận bị từ chối `BUSY` khi nhận invite hoặc tạo phòng mới; không cho phép một user tồn tại trong hai party; giải tán party phải cập nhật state cho người còn lại (Review Items 5, 10).
3. **Session Lifecycle & 24h Expiry:** Token quá 24h (`24 * 3600 * 1000` ms) khi gọi `RESUME` phải bị xóa và trả `UNAUTHENTICATED`; đăng nhập mới phải ngắt kết nối slot cũ ngay lập tức (Review Item 7, 14.7).
4. **Desktop Security & Navigation Containment:** WebKitGTK chỉ load origin `uno://app`, chặn tuyệt đối điều hướng ra ngoài hoặc mở URL từ biến môi trường; JS trong Webview không nhận được token thô (Review Items 3, 4, 12).
5. **Profile State Integrity & Streak Reset:** Thua trận đặt chuỗi thắng về giá trị âm (ví dụ -1), không cộng dồn streak cũ; bestStreak giữ nguyên; peakElo ghi đè; huy hiệu mở khóa ghi nhận đúng bảng `achievements` mà không nhân đôi khi finalize lại (Review Item 11).

---

## File Structure & Interfaces

```
shared/
  contracts.h           # Modify: Thêm BadgeBit, ProfileStats, cấu trúc lại ProfileDelta
Makefile                # Modify: Tách GTK_CFLAGS/GTK_LIBS, chỉ link GTK cho uno-desktop và test_bridge
server/
  app.h, app.c          # Modify: Token bucket rate limiter, đóng slow peer cuối vòng poll, sửa SERVER_FULL error envelope, bỏ double store_open
  schema.sql            # Modify: Thêm bảng profile_stats và achievements (bỏ schema key-value cũ)
  store.h, store.c      # Modify: store_finalize cập nhật profile_stats và achievements, thêm store_profile_stats
  accounts.h, accounts.c# Modify: Token hết hạn 24h, phân biệt STORAGE_ERROR khi đăng ký, đăng nhập mới ngắt kết nối cũ
  rooms.h, rooms.c      # Modify: Chặn vào 2 phòng (BUSY), kiểm tra ROOM_START status/on_start, dọn invite 30s, chặn vào 2 party & broadcast khi rời
desktop/
  bridge.h, bridge.c    # Modify: Cập nhật opcode whitelist chuẩn spec, lọc bỏ token trước khi emit JS, bỏ fallback spike_test
  main.c                # Modify: Đổi default timeout = 0, bỏ --uri/UNO_URI, chặn navigation ngoài uno://, xử lý G_IO_OUT
tests/
  test_bridge.c         # Modify: Kiểm tra opcode whitelist mới và giấu token
  test_net.c            # Modify: Thêm unit test token bucket rate limiter
  test_accounts.c       # Modify: Thêm test token 24h expired, new login revokes old slot
  test_rooms.c          # Modify: Thêm test BUSY khi đã ở phòng, double start check, invite 30s expiry
  test_store.c          # Modify: Thêm test profile_stats, streak reset âm, peakElo ghi đè, badge bitmask
  integration/
    test_transport.py   # Modify: Thêm test slow peer bị đóng thật, client drop giữa frame, SERVER_FULL format
    test_auth.py        # Modify: Thêm test DB error trả STORAGE_ERROR, test new login disconnects old conn
    test_rooms.py       # Modify: Thêm test 2 user tranh ghế cuối, test party leave broadcast
```

---

### Task 1: Makefile Decoupling & Desktop/Bridge Hardening

**Mục tiêu giải quyết:** Review Items 1, 3, 4, 12, 15, 16.2, 16.3.

**Files:**
- Modify: `Makefile:1-35`
- Modify: `desktop/bridge.h`, `desktop/bridge.c:30-80,150-185`
- Modify: `desktop/main.c:55-125`
- Modify: `tests/test_bridge.c`

**Interfaces:**
- Consumes:
  - `desktop/tcp_client.h`: `Client`, `client_send`, `client_read`
  - `shared/protocol.h`: `Envelope`, `Status`
- Produces:
  - `Makefile`: `$(BUILD)/uno-server` và các test không phải `test_bridge` chỉ dùng `BASE_CPPFLAGS` và `BASE_LDLIBS` (`json-c`, `sqlite3`, `libsodium`, `-pthread`). Riêng `$(BUILD)/uno-desktop` và `$(BUILD)/tests/test_bridge` dùng `GTK_CFLAGS` và `GTK_LIBS`.
  - `desktop/bridge.c`: Whitelist opcodes gồm đúng 25 lệnh chuẩn spec §3. `bridge_emit` xóa key `"token"` khỏi payload trước khi gửi cho Webview.
  - `desktop/main.c`: `timeout_sec = 0` mặc định; loại bỏ CLI flag `--uri` và biến môi trường `UNO_URI`; thêm WebKit handler `decide-policy` từ chối mọi navigation URI không có prefix `uno://app/`; bật `G_IO_OUT` cho socket để flush partial writes.

- [x] **Step 1: Viết test failing `tests/test_bridge.c`**

Trong `tests/test_bridge.c`:
- Bổ sung test opcode whitelist: xác nhận các lệnh spec (`LOGIN`, `REGISTER`, `ROOM_MODE`, `LOGOUT`, `QUEUE_JOIN`, `PLAY`, `DRAW`, `PASS`, `SAY_UNO`, `DRAFT_PICK`, `CHAT`) được chấp nhận (`OK`), và các opcode cũ/sai (`AUTH_LOGIN`, `AUTH_REGISTER`, `AUTH_GUEST`, `INVITE_RESPOND`, `GAME_ACTION`, `SPIKE_DONE`) bị từ chối (`BAD_REQUEST`).
- Bổ sung test `test_bridge_emit_strips_token`: truyền Envelope `type = "OK"` có payload chứa `token: "secret123"`, gọi hàm serialize và xác nhận chuỗi phát ra không chứa `"token"` hay `"secret123"`.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `wsl make test NAME=bridge`
Expected: Fail do whitelist cũ còn chứa `AUTH_LOGIN` và chưa có logic lọc token.

- [x] **Step 3: Cài đặt tách Makefile, cập nhật Bridge & Desktop**

1. `Makefile`:
   - Định nghĩa `BASE_CPPFLAGS := -I. -Ishared $(shell pkg-config --cflags json-c sqlite3 libsodium)`
   - Định nghĩa `BASE_LDLIBS := $(shell pkg-config --libs json-c sqlite3 libsodium) -pthread`
   - Chỉ gắn `GTK_CFLAGS` và `GTK_LIBS` vào mục tiêu `desktop` và `test_bridge`.
   - Bỏ `$(desktop_modules)` khỏi rule build test chung `$(BUILD)/tests/test_%: tests/test_%.c $(shared) $(modules) $(headers)`.
2. `desktop/bridge.c`:
   - Thay mảng `whitelist[]` bằng đúng 25 opcode chuẩn của spec §3.
   - Trong `bridge_emit`: nếu `json_object_object_get(e->payload, "token")` tồn tại, tạo bản sao payload loại bỏ trường `"token"` trước khi gửi vào `webkit_web_view_evaluate_javascript`.
   - Xóa bỏ fallback file `tests/fixtures/spike_test.html` dư thừa.
3. `desktop/main.c`:
   - Đặt `int timeout_sec = 0;` mặc định.
   - Xóa bỏ cờ `--uri` và env `UNO_URI`, cố định load `uno://app/index.html`.
   - Đăng ký signal `decide-policy` trên WebKitWebView: kiểm tra `webkit_navigation_policy_decision_get_navigation_action`, nếu URI không bắt đầu bằng `uno://app` thì gọi `webkit_policy_decision_ignore(decision)`.
   - Thêm `G_IO_OUT` vào `g_io_add_watch`: khi socket ghi được, gọi `conn_flush` để tiếp tục gửi phần dở.
   - Cập nhật mục tiêu `desktop-smoke` trong `Makefile` truyền cờ `--timeout 5 --assets tests/fixtures`.

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `wsl make test NAME=bridge`
Expected: `bridge: OK`, exit 0.
Run: `wsl xvfb-run -a make desktop-smoke`
Expected: `desktop-smoke: OK`, exit 0.

- [x] **Step 5: Kiểm chứng tách biệt thư viện GUI**

Chạy thử biên dịch `uno-server` và một test engine:
Run: `wsl make server`
Run: `wsl make test NAME=game`
Expected: Biên dịch thành công mà không tham chiếu bất kỳ thư viện `gtk` hay `webkit` nào.

---

### Task 2: Transport Layer Resilience, Rate Limiting & Slow Peer Teardown

**Mục tiêu giải quyết:** Review Items 2, 13, 14.1, 14.2, 14.8.

**Files:**
- Modify: `server/app.h`, `server/app.c:300-390`
- Modify: `tests/test_net.c`
- Modify: `tests/integration/test_transport.py`

**Interfaces:**
- Consumes:
  - `shared/net.h`: `Conn`, `conn_queue`, `conn_flush`, `conn_close`
  - `shared/protocol.h`: `Envelope`, `frame_encode`, `Status`
- Produces:
  - `AppConnSlot`: Thêm trường `uint64_t rate_last_ms; double rate_tokens;` cho thuật toán Token Bucket (30 req/s, burst 60).
  - `server/app.c`: Kiểm tra rate limit trong `app_on_frame`: nếu hết token, gửi `ERROR {code: "RATE_LIMITED", message: "Too many requests"}` và bỏ qua lệnh.
  - `server/app.c`: Sau vòng lặp xử lý sự kiện `poll()`, duyệt mảng slot: nếu `slot->active && slot->conn.closing`, gọi ngay `app_close_slot(app, i)` để ngắt slow peer không phụ thuộc vào `POLLIN`/`POLLOUT`.
  - `server/app.c`: Khi server quá tải 64 kết nối, gửi về client frame:
    `Envelope { v: 1, id: 0, type: "ERROR", payload: { code: "SERVER_FULL", message: "Server is full" } }`.

- [x] **Step 1: Viết failing test trong `tests/test_net.c` và `tests/integration/test_transport.py`**

Trong `tests/test_net.c`:
- `test_token_bucket_rate_limiter`: mô phỏng nạp token và tiêu thụ: burst 60 request liên tiếp trong 0ms thành công, request thứ 61 trả về `RATE_LIMITED`. Sau 100ms tích lũy thêm 3 token, gửi tiếp 3 request thành công.

Trong `tests/integration/test_transport.py`:
- `test_slow_consumer_disconnect`: mở 1 client, gửi liên tục lượng frame lớn từ server khiến buffer vượt 262144 bytes trong khi client không đọc socket -> xác nhận server tự động đóng kết nối client này (phát hiện socket EOF/Reset trên client), và các client khác vẫn hoạt động bình thường.
- `test_client_disconnect_mid_frame`: client kết nối, gửi 4 byte length header (`0x00, 0x00, 0x00, 0x50`) rồi đóng socket ngay lập tức -> xác nhận server không crash, giải phóng slot sạch sẽ.
- `test_max_connections_error_format`: cập nhật assertion kiểm tra client thứ 65 nhận được gói tin có `type == "ERROR"` và `payload.code == "SERVER_FULL"`.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `wsl make test NAME=net` và `wsl make integration NAME=transport`
Expected: Fail do chưa có token bucket limiter và format `SERVER_FULL` đang là `type: "SERVER_FULL"`.

- [x] **Step 3: Cài đặt Rate Limiting & Dọn dẹp Slow Peer**

1. Cập nhật `server/app.h`:
   - Thêm vào `AppConnSlot`: `uint64_t rate_last_ms; double rate_tokens;`.
2. Cập nhật `server/app.c`:
   - Trong `app_on_frame`: tính khoảng thời gian `now_ms - slot->rate_last_ms`. Nạp thêm `elapsed * 0.03` token (tương đương 30 token/giây), clamp tối đa 60.0. Nếu `rate_tokens < 1.0`, gửi phản hồi `ERROR` với mã `RATE_LIMITED` và dừng xử lý request. Ngược lại trừ đi 1.0 token.
   - Trong vòng lặp `poll`: tại cuối mỗi iteration, quét tất cả slot `0..APP_MAX_CONNS-1`:
     ```c
     for (int i = 0; i < APP_MAX_CONNS; i++) {
         if (app->slots[i].active && app->slots[i].conn.closing) {
             app_close_slot(app, i);
         }
     }
     ```
   - Sửa phản hồi từ chối client thứ 65:
     ```c
     struct json_object *payload = json_object_new_object();
     json_object_object_add(payload, "code", json_object_new_string("SERVER_FULL"));
     json_object_object_add(payload, "message", json_object_new_string("Server connection limit reached"));
     Envelope full_env = {1, 0, "ERROR", payload};
     ```

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `wsl make test NAME=net`
Expected: `net: OK`, exit 0.
Run: `wsl make integration NAME=transport`
Expected: Tất cả các case slow consumer, mid-frame disconnect, max connections pass 100%.

- [x] **Step 5: Kiểm tra ASan/UBSan**

Run: `wsl make sanitize NAME=net`
Expected: Sạch memory leak và descriptor.

---

### Task 3: Account Lifecycle, Session Expiry & Registration Errors

**Mục tiêu giải quyết:** Review Items 7, 8, 14.3, 14.7, 16.5.

**Files:**
- Modify: `server/accounts.h`, `server/accounts.c:290-340,600-640`
- Modify: `server/auth_worker.c`
- Modify: `tests/test_accounts.c`
- Modify: `tests/integration/test_auth.py`

**Interfaces:**
- Consumes:
  - `server/store.h`: `store_account_create`, `store_account_get`
  - `server/app.h`: `App`, `AppConnSlot`, `app_close_slot`
- Produces:
  - `sessions_resume`: So sánh `now_ms - session->created_at_ms`. Nếu `> 24 * 3600 * 1000` (86.400.000 ms), xóa session và trả về `UNAUTHENTICATED`.
  - `auth_drain`: Phân biệt mã lỗi trả về từ `store_account_create`: nếu `BAD_REQUEST` trả `BAD_REQUEST ("Email already registered")`; nếu lỗi khác trả `STORAGE_ERROR ("Database storage failure")`.
  - `auth_drain` (khi `LOGIN` thành công): Nếu user này đã có kết nối đang active ở slot khác, gọi ngay `app_close_slot` ngắt kết nối cũ trước khi gán kết nối mới (đúng spec: "một phiên active/user, login mới thay connection cũ").
  - `auth_worker.c`: Gọi trực tiếp hàm sinh token `accounts_generate_token()` thay vì tự sinh riêng biệt.

- [x] **Step 1: Viết failing test trong `tests/test_accounts.c` và `tests/integration/test_auth.py`**

Trong `tests/test_accounts.c`:
- `test_token_24h_expiration`: tạo session với `created_at_ms = now - 24*3600*1000 - 1000` (đã quá 24h). Gọi `sessions_resume` -> xác nhận trả về `UNAUTHENTICATED` và slot session bị xóa.
- `test_token_within_24h_valid`: tạo session với `created_at_ms = now - 23*3600*1000`. Gọi `sessions_resume` -> xác nhận trả về `OK`.

Trong `tests/integration/test_auth.py`:
- `test_new_login_disconnects_old_connection`: Client 1 đăng nhập thành công. Client 2 từ tiến trình khác đăng nhập cùng tài khoản đó -> xác nhận Client 2 đăng nhập thành công (`OK`), và socket của Client 1 bị server ngắt kết nối (nhận EOF).
- `test_register_db_error_reports_storage_error`: cấu hình kiểm tra trường hợp DB gặp lỗi ghi (hoặc file read-only) -> xác nhận trả về mã lỗi `STORAGE_ERROR` chứ không báo nhầm là trùng email.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `wsl make test NAME=accounts` và `wsl make integration NAME=auth`
Expected: Fail do chưa có logic kiểm tra 24h và chưa ngắt kết nối cũ khi login mới.

- [x] **Step 3: Cài đặt kiểm tra thời hạn Token & Quản lý phiên**

1. Cập nhật `server/accounts.c`:
   - Trong `sessions_resume`:
     ```c
     uint64_t now = get_now_ms();
     if (now > s->entries[i].created_at_ms && (now - s->entries[i].created_at_ms) > 24ULL * 3600ULL * 1000ULL) {
         memset(&s->entries[i], 0, sizeof(s->entries[i]));
         return UNAUTHENTICATED;
     }
     ```
   - Trong `auth_drain` khi đăng ký:
     ```c
     if (db_st == OK) {
         // send OK
     } else if (db_st == BAD_REQUEST) {
         accounts_send_error(app, slot_idx, res.req_id, "BAD_REQUEST", "Email already registered");
     } else {
         accounts_send_error(app, slot_idx, res.req_id, "STORAGE_ERROR", "Database storage failure");
     }
     ```
   - Trong `auth_drain` khi đăng nhập (`AUTH_LOGIN`):
     Tìm xem `user_id` đã có session kết nối cũ chưa (`sessions_find_conn`). Nếu có và slot cũ đang active (`slot_old != slot_new`), gọi `app_close_slot(app, slot_old)` để ngắt kết nối cũ ngay lập tức trước khi `sessions_bind`.
2. Cập nhật `server/auth_worker.c`:
   - Dùng `accounts_generate_token()` để sinh session token duy nhất.

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `wsl make test NAME=accounts`
Expected: `accounts: OK`, exit 0.
Run: `wsl make integration NAME=auth`
Expected: Toàn bộ test auth pass 100%.

- [x] **Step 5: Kiểm tra ASan/UBSan**

Run: `wsl make sanitize NAME=accounts`
Expected: Sạch bộ nhớ.

---

### Task 4: Room, Party & Invite Invariants

**Mục tiêu giải quyết:** Review Items 5, 6, 9, 10, 14.4, 14.5, 14.6, 16.4, 17, 18.

**Files:**
- Modify: `server/rooms.h`, `server/rooms.c`
- Modify: `tests/test_rooms.c`
- Modify: `tests/integration/test_rooms.py`

**Interfaces:**
- Consumes:
  - `shared/contracts.h`: `Status`, `Seat`, `UserId`, `Mode`, `BotLevel`
  - `server/accounts.h`: `sessions_user`, `accounts_send_push`
- Produces:
  - `rooms_create` & `rooms_join`: Nếu `rooms_find_by_user(r, user_id) != NULL`, trả về `BUSY` ("User already in a room"). Tuyệt đối không tự động gọi `rooms_leave`.
  - `rooms_start`: Kiểm tra `room->status == ROOM_STATUS_WAITING` (nếu không, trả `INVALID_ACTION`) và kiểm tra `r->on_start != NULL` (nếu NULL, trả `BUSY`).
  - Invite slot allocation: Kiểm tra lời mời quá 30s (`(now_ms - created_at_ms) > 30000ULL`) được coi là slot trống. Dùng `CLOCK_MONOTONIC`.
  - Party accept: Kiểm tra `party_find_by_user(r, user_id) != NULL` -> trả về `BUSY` ("Already in a party").
  - `party_leave`: Broadcast push `PARTY_STATE` thông báo giải tán party tới cả 2 thành viên trước khi zero vùng nhớ.
  - Bounds check: Validate `seat_val >= 0 && seat_val < ROOM_MAX_SEATS` trước khi ép kiểu `(Seat)seat_val` trong `ROOM_BOT` và `ROOM_KICK`.
  - Dọn biến global `g_last_rooms` trong `rooms.c`.

- [x] **Step 1: Viết failing test trong `tests/test_rooms.c` và `tests/integration/test_rooms.py`**

Trong `tests/test_rooms.c`:
- `test_user_in_room_cannot_create_or_join_another`: User 1 tạo Phòng A. User 1 gửi tiếp `rooms_create` -> trả về `BUSY`. User 1 gửi `rooms_join` vào Phòng B -> trả về `BUSY`. Phòng A vẫn giữ nguyên User 1 ở ghế 0.
- `test_room_start_invariants`: Phòng chưa đủ người gọi start trả `INVALID_ACTION`. Phòng đủ 4 người nhưng `on_start == NULL` trả `BUSY`. Phòng đã ở `ROOM_STATUS_IN_MATCH` gọi start lần 2 trả `INVALID_ACTION`.
- `test_invite_30s_slot_reclamation`: Tạo 64 lời mời với timestamp cách đây 31 giây. Tạo lời mời thứ 65 -> xác nhận tạo thành công vào slot đã hết hạn mà không ghi đè mất lời mời còn hạn.
- `test_seat_bounds_validation`: gọi `rooms_set_bot` hoặc `rooms_kick` với `seat = 4` hoặc `seat = 257` -> trả về `BAD_REQUEST`.

Trong `tests/integration/test_rooms.py`:
- `test_race_for_last_seat`: Phòng có 3 người (1 ghế trống cuối). Hai client gửi `ROOM_JOIN` đồng thời -> xác nhận đúng 1 client nhận `OK` (ghế 3), client còn lại nhận `ROOM_FULL`.
- `test_party_leave_broadcast`: A và B trong 1 party. B gửi `PARTY_LEAVE` -> xác nhận cả A và B đều nhận được push thông báo party đã giải tán.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `wsl make test NAME=rooms` và `wsl make integration NAME=rooms`
Expected: Fail do các điều kiện kiểm tra `BUSY`, double start và bounds chưa được cài đặt.

- [x] **Step 3: Cài đặt logic phòng, party và invite**

1. Cập nhật `get_now_ms()` trong `server/rooms.c`: dùng `clock_gettime(CLOCK_MONOTONIC, &ts)`.
2. Cập nhật `rooms_create` và `rooms_join`:
   - Bỏ lệnh gọi `rooms_leave(r, user_id)`.
   - Kiểm tra `if (rooms_find_by_user(r, user_id) != NULL) return BUSY;`.
3. Cập nhật `rooms_start`:
   - `if (room->status != ROOM_STATUS_WAITING) return INVALID_ACTION;`
   - `if (!r->on_start) return BUSY;`
4. Cập nhật tìm slot invite:
   - Coi slot có `!inv->active || (now - inv->created_at_ms > 30000ULL)` là slot trống.
5. Cập nhật chấp nhận lời mời:
   - Lời mời vào phòng: nếu người nhận đang ở phòng khác, trả `BUSY`.
   - Lời mời vào party: nếu người nhận đang ở party khác, trả `BUSY`.
6. Cập nhật `party_leave`:
   - Trước khi `memset(p, 0, sizeof(*p))`, phát push `PARTY_STATE` với danh sách thành viên rỗng tới cả 2 user.
7. Cập nhật kiểm tra seat trong `ROOM_BOT` và `ROOM_KICK`:
   - `if (seat_val < 0 || seat_val >= ROOM_MAX_SEATS) { send_error(app, conn_id, e->id, "BAD_REQUEST", "Seat must be between 0 and 3"); return OK; }`
8. Xóa bỏ biến toàn cục `g_last_rooms`.

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `wsl make test NAME=rooms`
Expected: `rooms: OK`, exit 0.
Run: `wsl make integration NAME=rooms`
Expected: Toàn bộ integration test phòng, invite và party pass 100%.

- [x] **Step 5: Kiểm tra ASan/UBSan**

Run: `wsl make sanitize NAME=rooms`
Expected: Sạch bộ nhớ.

---

### Task 5: ProfileDelta Contract, Schema & Store Overhaul

**Mục tiêu giải quyết:** Review Items 11, 16.1, 16.6, 16.7.

**Files:**
- Modify: `shared/contracts.h:60-70`
- Modify: `server/schema.sql`
- Modify: `server/store.h`, `server/store.c:20-60,350-480`
- Modify: `server/app.c:155-165,265-275,310-320`
- Modify: `tests/test_store.c`

**Interfaces:**
- Consumes:
  - `shared/contracts.h`: `BadgeBit`, `ProfileStats`, `ProfileDelta`, `MatchResult`, `Status`
- Produces:
  - `shared/contracts.h`:
    - `BadgeBit` enum: `BADGE_FIRST_WIN (1<<0)`, `BADGE_WIN_100 (1<<1)`, `BADGE_STREAK_10 (1<<2)`, `BADGE_PLUS4 (1<<3)`, `BADGE_UNO_MASTER (1<<4)`, `BADGE_TEAM (1<<5)`, `BADGE_MATCHES (1<<6)`, `BADGE_DIAMOND (1<<7)`.
    - `ProfileStats`: `uint32_t matches, wins, teamWins, plus4, unoCalls; int32_t streak; uint32_t bestStreak; int32_t peakElo;`.
    - `ProfileDelta`: `struct { uint32_t matches, wins, teamWins, plus4, unoCalls; } add; int32_t streak; uint32_t bestStreak; int32_t peakElo; uint32_t unlocked;`.
  - `server/store.h`:
    - `Status store_profile_stats(Store *s, UserId user_id, ProfileStats *out)`
    - `Status store_finalize(Store *s, const MatchResult *result, const ProfileDelta deltas[4])`
  - `server/app.c`:
    - Bỏ lần gọi `store_open` dư thừa ở `app_init` (chỉ mở một lần duy nhất tại `app_run`).
    - Chỉ gọi `auth_drain` một lần tại vị trí xử lý I/O pipe và cuối vòng lặp khi có frame.
    - Dùng chung mảng hằng số tên mode.

- [x] **Step 1: Viết failing test trong `tests/test_store.c`**

Trong `tests/test_store.c`:
- `test_profile_stats_initial_state`: tạo account, gọi `store_profile_stats` -> xác nhận `matches == 0, wins == 0, streak == 0, peakElo == 1500`.
- `test_profile_stats_finalize_accumulation_and_overwrite`:
  - Trận 1 (thắng): delta có `add.matches = 1, add.wins = 1, streak = 1, bestStreak = 1, peakElo = 1516, unlocked = BADGE_FIRST_WIN`. Gọi `store_finalize` -> xác nhận `matches = 1, wins = 1, streak = 1, bestStreak = 1, peakElo = 1516`, badge `BADGE_FIRST_WIN` được lưu trong DB.
  - Trận 2 (thua): delta có `add.matches = 1, add.wins = 0, streak = -1, bestStreak = 1, peakElo = 1516, unlocked = 0`. Gọi `store_finalize` -> xác nhận `matches = 2, wins = 1, streak = -1, bestStreak = 1` (giữ nguyên chuỗi thắng tốt nhất), `peakElo = 1516` (không bị cộng dồn thành 3032).
  - Trận 3 (thắng đạt Kim Cương): delta có `peakElo = 1805, unlocked = BADGE_DIAMOND`. Gọi `store_finalize` -> xác nhận `peakElo = 1805`, huy hiệu `BADGE_DIAMOND` được mở.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `wsl make test NAME=store`
Expected: Fail biên dịch do `ProfileDelta` và `ProfileStats` chưa được cập nhật trong `contracts.h`.

- [x] **Step 3: Cài đặt Contract mới, Schema và Store Finalize**

1. Cập nhật `shared/contracts.h`:
   - Thêm `BadgeBit`, `ProfileStats`, và định nghĩa lại `ProfileDelta` theo đúng thiết kế chốt 15/11.
2. Cập nhật `server/schema.sql` và mã nhúng trong `server/store.c`:
   - Xóa bảng `achievements(key, progress, ...)` dạng key-value cũ.
   - Thêm bảng `profile_stats`:
     ```sql
     CREATE TABLE IF NOT EXISTS profile_stats (
         user_id INTEGER PRIMARY KEY REFERENCES accounts(id),
         matches INTEGER NOT NULL DEFAULT 0,
         wins INTEGER NOT NULL DEFAULT 0,
         team_wins INTEGER NOT NULL DEFAULT 0,
         plus4 INTEGER NOT NULL DEFAULT 0,
         uno_calls INTEGER NOT NULL DEFAULT 0,
         streak INTEGER NOT NULL DEFAULT 0,
         best_streak INTEGER NOT NULL DEFAULT 0,
         peak_elo INTEGER NOT NULL DEFAULT 1500
     );
     ```
   - Thêm bảng `achievements`:
     ```sql
     CREATE TABLE IF NOT EXISTS achievements (
         user_id INTEGER NOT NULL,
         badge INTEGER NOT NULL,
         unlocked_at INTEGER NOT NULL,
         match_id TEXT NOT NULL,
         PRIMARY KEY (user_id, badge)
     );
     ```
3. Cập nhật `server/store.c`:
   - Trong `store_account_create`: insert thêm 1 dòng mặc định vào `profile_stats(user_id, peak_elo) VALUES (?, 1500)`.
   - Cài đặt `store_profile_stats(Store *s, UserId user_id, ProfileStats *out)`: đọc từ bảng `profile_stats`.
   - Trong `store_finalize`:
     Với mỗi player (nếu không phải bot, `userId > 0`):
     ```sql
     UPDATE profile_stats
        SET matches = matches + ?,
            wins = wins + ?,
            team_wins = team_wins + ?,
            plus4 = plus4 + ?,
            uno_calls = uno_calls + ?,
            streak = ?,
            best_streak = ?,
            peak_elo = ?
      WHERE user_id = ?;
     ```
     Duyệt các bit `1 << b` trong `deltas[i].unlocked`:
     Nếu bit bật, thực thi:
     ```sql
     INSERT OR IGNORE INTO achievements (user_id, badge, unlocked_at, match_id)
     VALUES (?, ?, ?, ?);
     ```
4. Dọn dẹp trong `server/app.c`:
   - Bỏ lệnh gọi `store_open` trùng lặp trong `app_init`.
   - Gọi `auth_drain` tối ưu (khi có sự kiện pipe và sau khi xử lý frame).

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `wsl make test NAME=store`
Expected: `store: OK`, exit 0.

- [x] **Step 5: Kiểm tra ASan/UBSan**

Run: `wsl make sanitize NAME=store`
Expected: Không có rò rỉ bộ nhớ hoặc lỗi SQLite statement.

---

### Task 6: Full Regression Verification & Clean Merge Gate

**Mục tiêu giải quyết:** Kiểm chứng toàn bộ hệ thống sau khi sửa 18 lỗi review, đảm bảo không có bất kỳ regression nào trên toàn bộ unit test, integration test, và desktop smoke test.

**Files:**
- Toàn bộ codebase.

- [x] **Step 1: Chạy toàn bộ Unit Tests**

Run: `wsl make test`
Expected: Toàn bộ 9/9 test suite (`accounts`, `bot`, `bridge`, `game`, `modes`, `net`, `protocol`, `rooms`, `store`, `view`) đều in `OK` và thoát exit code 0.

- [x] **Step 2: Chạy Desktop Smoke Test**

Run: `wsl xvfb-run -a make desktop-smoke`
Expected: `desktop-smoke: OK`, exit code 0.

- [x] **Step 3: Chạy toàn bộ Integration Tests**

Run: `wsl make integration NAME=transport`
Expected: 100% pass (bao gồm test slow peer và mid-frame disconnect).
Run: `wsl make integration NAME=auth`
Expected: 100% pass (bao gồm 24h expiration và new login disconnects old slot).
Run: `wsl make integration NAME=rooms`
Expected: 100% pass (bao gồm race tranh ghế cuối, invite 30s expiry và party leave broadcast).

- [x] **Step 4: Chạy Memory Sanitizer toàn diện**

Run: `wsl make sanitize`
Expected: Tất cả các bài test đều pass dưới ASan và UBSan, không rò rỉ bộ nhớ hoặc lỗi handle.

---

## Tự rà soát bản plan (Self-Review Checklist)

1. **Spec & Review Coverage:**
   - Đã xử lý toàn bộ 5 lỗi bắt buộc trước merge (Items 1–5: Makefile decoupling, slow peer teardown, desktop timeout default, bridge whitelist opcodes, phòng BUSY).
   - Đã xử lý toàn bộ 10 lỗi trước Task 9 (Items 6–15: ROOM_START guards, token 24h expiry, register DB errors, invite 30s reclamation, party dual-membership, ProfileDelta overhaul, bridge token privacy, rate limit 30 req/s, tests missing, desktop partial flush).
   - Đã xử lý 3 khuyến nghị cleanups (Items 16–18: xóa code thừa, CLOCK_MONOTONIC cho invites, validate seat bounds 0..3).
2. **Step Scan:** Mỗi task được chia thành 5 bước TDD: Viết failing test -> Chạy đỏ -> Cài đặt tối thiểu -> Chạy xanh -> Sanitize / Integration.
3. **Type Consistency:** Tuân thủ cấu trúc dữ liệu mới `ProfileStats`, `ProfileDelta`, `BadgeBit`, bảo toàn kiểu dữ liệu trong `shared/contracts.h`.
4. **Review Focus:** 5 điểm trọng yếu nhất (slow peer, phòng/party state, token 24h, WebKit security, streak/peakElo) đều có test cụ thể.
5. **Proportion:** Kế hoạch tập trung trực tiếp vào các quyết định kiến trúc và interface, kiểm thử rõ ràng, súc tích.
