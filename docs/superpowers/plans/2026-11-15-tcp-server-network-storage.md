# UNO TCP Server Infrastructure & Spike 12 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hoàn thành toàn bộ hạ tầng mạng, lưu trữ và sảnh phía Sơn (Task 2, Task 3, Task 4, Task 5) cùng một bản Spike kỹ thuật cho Task 12 (WebKitGTK C Bridge) để đảm bảo thông tuyến toàn diện kiến trúc UNO Ubuntu TCP.

**Architecture:** Server C một luồng (single-threaded) điều phối bằng `poll()` trên socket non-blocking phục vụ tối đa 64 kết nối và 16 phòng. Tách biệt mật khẩu tốn CPU sang 1 pthread worker thông báo qua non-blocking pipe. SQLite3 quản lý ACID transaction cho match events, account và profile với WAL mode. Client desktop GTK3/WebKitGTK giao tiếp nội bộ qua URI scheme `uno://app` và WebKit message handler, chuyển tiếp TCP client C tới server.

**Tech Stack:** Ubuntu 24.04 LTS amd64; C11, GCC, GNU Make; POSIX Sockets, `poll()`, `pthreads`; `json-c`, `libsqlite3`, `libsodium`; GTK3 + WebKitGTK 4.1 (`webkit2gtk-4.1`); Python 3 (`unittest`) cho integration tests.

**Spec:** [docs/superpowers/specs/2026-11-11-ubuntu-tcp-design.md](../specs/2026-11-11-ubuntu-tcp-design.md) và [docs/superpowers/plans/2026-11-11-ubuntu-tcp-uno.md](2026-11-11-ubuntu-tcp-uno.md).

## Global Constraints

- Môi trường thực thi: Ubuntu 24.04 LTS amd64; server và client mạng viết bằng C dùng POSIX TCP socket; không WebSocket.
- Tuân thủ nghiêm ngặt framing UNOP/1: 4-byte unsigned big-endian length + JSON UTF-8 payload (1..65536 bytes). Parser bằng `json-c`, max depth 16, không trailing garbage.
- Server-authoritative: Server sở hữu state, timer, room, auth và SQLite. Session xác thực người chơi, client không gửi actor/seat giả lập.
- Giới hạn hệ thống: Tối đa 64 connection, 16 phòng; output buffer tối đa 256 KiB (`262144` bytes) mỗi connection (đóng client chậm khi vượt quá); rate limit mạng 30 req/s (burst 60); auth rate limit mặc định 5/phút/IP (hỗ trợ cờ `--auth-limit N` cho integration test).
- Auth worker: 1 pthread worker duy nhất với queue tối đa 8 jobs; kết quả trả về pipe không giữ con trỏ session; kiểm tra `ConnId` (slot + generation) chống gửi nhầm kết quả cho socket tái sử dụng.
- Lưu trữ SQLite: PRAGMA `foreign_keys = ON;`, `busy_timeout = 100;`, WAL mode; recovery active -> aborted khi khởi động; không DROP bảng để reset test; finalize và events chạy trong single atomic transaction.
- Quy ước kiểm thử: `make test NAME=x` chạy `build/tests/test_x`; `make integration NAME=x` chạy `python3 -m unittest tests.integration.test_x -v`; `make sanitize NAME=x` kiểm tra ASan/UBSan.

## Review Focus

1. **TCP Fragmented / Coalesced Frames & Slow Peer:** Nhận từng byte lẻ hoặc nhiều frame dính nhau không được mất gói; peer đọc chậm làm output buffer vượt 256 KiB phải bị đóng kết nối mà không làm treo các phòng khác (Task 2).
2. **WebKitGTK URI Scheme & Bridge Boundary Traversal:** Custom scheme `uno://app` chặn tuyệt đối path traversal (`../`) và không cho phép JS thực thi mã độc hoặc gọi API hệ thống tùy tiện (Task 2.5 Spike 12).
3. **Database Transaction Failure & Reopen Crash Recovery:** Crash giữa trận hoặc lỗi ghi đĩa khi finalize trận không được làm hỏng DB, không ghi lặp ELO, và khi server khởi động lại phải đổi trạng thái trận cũ từ active sang aborted (Task 3).
4. **Auth Worker Concurrency & Fd Reuse Race:** Hash mật khẩu mất thời gian; nếu client ngắt kết nối hoặc slot fd bị client khác nhận trước khi worker trả kết quả qua pipe, server phải hủy kết quả an toàn dựa trên `generation`, không cấp nhầm session (Task 4).
5. **Room Host Privileges & Seat Race:** Hai client cùng tranh ghế cuối hoặc gửi lệnh bắt đầu phòng cùng lúc: chỉ một client vào được ghế (trả `ROOM_FULL` cho người kia), lệnh không phải từ host phải trả `FORBIDDEN` (Task 5).

---

## Cấu trúc file và Interfaces

```
shared/
  contracts.h           # Đã có (Task 1) - types chung: Status, UserId, Seat, Mode, PlayerSpec, EventBatch, MatchResult, ProfileDelta
  protocol.h, protocol.c # Đã có (Task 1) - framing và envelope JSON
  net.h, net.c          # Task 2: Conn struct, non-blocking socket I/O, output buffering
server/
  app.h, app.c          # Task 2, 3, 4, 5: App struct, poll loop, dispatch, session map
  main.c                # Task 2: CLI entry point (--bind, --port, --db, --auth-limit)
  schema.sql            # Task 3: SQLite schema version 1
  store.h, store.c      # Task 3: Database repository API
  auth_worker.h, .c     # Task 4: Pthread password hashing worker & pipe
  accounts.h, accounts.c# Task 4: Account validation, session management, token handling
  rooms.h, rooms.c      # Task 5: Room lifecycle, host permissions, invites, parties
desktop/
  tcp_client.h, .c      # Task 2: Headless POSIX TCP client
  bridge.h, bridge.c    # Task 2.5: WebKitGTK URI scheme & JS bridge
  main.c                # Task 2.5: GTK3 window + WebKitWebView runner
tests/
  test_net.c            # Task 2: Unit test non-blocking network buffers & poll
  test_bridge.c         # Task 2.5: Unit test bridge message dispatch & scheme security
  test_store.c          # Task 3: Unit test SQLite match transactions & idempotency
  test_accounts.c       # Task 4: Unit test account validation & session state
  test_rooms.c          # Task 5: Unit test room state, bot seats, host migration
  integration/
    helpers.py          # Task 2: ServerProcess & WireClient helpers
    test_transport.py   # Task 2: Integration test connection limits & slow consumers
    test_auth.py        # Task 4: Integration test register/login concurrency & rate limits
    test_rooms.py       # Task 5: Integration test room creation, join race & invites
```

---

### Task 2: TCP listener và client C headless — Sơn

**Files:**
- Create: `shared/net.c`, `shared/net.h`
- Create: `server/app.c`, `server/app.h`, `server/main.c`
- Create: `desktop/tcp_client.c`, `desktop/tcp_client.h`
- Create: `tests/test_net.c`
- Create: `tests/integration/__init__.py`, `tests/integration/helpers.py`, `tests/integration/test_transport.py`
- Modify: `Makefile`

**Interfaces:**
- Consumes:
  - `shared/protocol.h`: `frame_feed`, `frame_encode`, `FrameReader`, `Envelope`, `FRAME_MAX_JSON`, `FRAME_CAPACITY`
  - `shared/contracts.h`: `Status`, `ConnId`
- Produces:
  - `Status conn_init(Conn *c, int fd, uint32_t slot, uint32_t generation)`
  - `Status conn_read(Conn *c, FrameCallback cb, void *ctx)`
  - `Status conn_queue(Conn *c, const Envelope *e)`
  - `Status conn_flush(Conn *c)`
  - `void conn_close(Conn *c)`
  - `Status client_connect(Client **out, const char *host, uint16_t port)`
  - `Status client_send(Client *client, const Envelope *e)`
  - `Status client_poll(Client *client, FrameCallback cb, void *ctx, int timeout_ms)`
  - `int client_fd(const Client *client)`
  - `void client_close(Client *client)`
  - `int app_run(const char *bind_ip, uint16_t port, const char *db_path, int auth_limit)`

- [x] **Step 1: Viết test failing `tests/test_net.c` và integration test `tests/integration/test_transport.py`**

Trong `tests/test_net.c`:
- Sử dụng `socketpair(AF_UNIX, SOCK_STREAM, 0, sv)` với `SO_SNDBUF` = 2048 bytes để ép socket rơi vào `EAGAIN`.
- Viết `test_partial_write_and_flush`: gửi frame lớn hơn send buffer, xác nhận `conn_queue` và `conn_flush` trả `OK`, lưu lại offset và khi đọc bớt bên nhận thì flush tiếp tục gửi hết data mà không mất mát.
- Viết `test_slow_consumer_threshold`: queue dồn vượt 262144 bytes (256 KiB), xác nhận hàm trả về `RATE_LIMITED` hoặc lỗi đóng connection.

Trong `tests/integration/helpers.py` và `test_transport.py`:
- `ServerProcess`: khởi động `build/uno-server --port 0 --bind 127.0.0.1` đọc actual port in ra stdout.
- `WireClient`: kết nối socket TCP gửi nhận frame 4-byte big-endian prefix + JSON UTF-8.
- `test_concurrent_clients`: mở 8 client, gửi các frame `HELLO` và `PING`, kiểm tra `id` response tương ứng.
- `test_coalesced_frames`: gửi 2 frame `HELLO` trong 1 lần `send`, nhận về đủ 2 response.
- `test_max_connections`: kết nối 64 client thành công; client thứ 65 bị từ chối với lỗi `SERVER_FULL` hoặc đóng kết nối ngay, trong khi client cũ vẫn `PING` bình thường.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `make test NAME=net`
Expected: Biên dịch lỗi do thiếu `shared/net.h`, `server/app.h` hoặc test runner fail.

- [x] **Step 3: Cài đặt mạng non-blocking, buffering và poll loop**

- Triển khai `shared/net.c`, `shared/net.h`:
  - `Conn` gồm: `int fd; uint32_t slot, generation; FrameReader reader; uint8_t out_buf[262144]; size_t out_len, out_pos; bool closing;`.
  - `conn_read`: đọc từ fd vào buffer nonblocking với `recv()`, xử lý `EAGAIN`/`EINTR`, chuyển qua `frame_feed()`.
  - `conn_queue`: encode `Envelope` bằng `frame_encode()`, chép vào `out_buf`. Nếu `out_len + n > 262144`, trả về `RATE_LIMITED` và đánh dấu `closing`.
  - `conn_flush`: gọi `send()`, dịch chuyển `out_pos` dựa theo số byte đã gửi.
  - `conn_close`: `close(fd)`, giải phóng bộ nhớ, reset generation.
- Triển khai `desktop/tcp_client.c`, `desktop/tcp_client.h`:
  - Non-blocking `connect()`, kiểm tra `SO_ERROR` qua `getsockopt()`, bọc `conn_read` và `conn_flush`.
- Triển khai `server/app.c`, `server/app.h`, `server/main.c`:
  - Listener socket với `O_NONBLOCK`, `SO_REUSEADDR`. Bỏ `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`).
  - Main loop sử dụng `poll(fds, nfds, timeout)`. Quản lý mảng tối đa 64 slot `Conn`.
  - Xử lý các command cơ bản: `HELLO` -> trả `OK { "serverTimeMs": ... }`; `PING` -> trả `OK {}`.
  - CLI parser cho `--bind`, `--port`, `--db`, `--auth-limit`.

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `make test NAME=net`
Expected: Output `net: OK`, exit code 0.
Run: `make integration NAME=transport`
Expected: Các test `test_concurrent_clients`, `test_coalesced_frames`, `test_max_connections` pass.

- [x] **Step 5: Kiểm tra ASan/UBSan**

Run: `make sanitize NAME=net`
Expected: Không có cảnh báo memory leak hay undefined behavior.

---

### Task 2.5 (Spike Task 12): GTK3/WebKitGTK & TCP Bridge Spike — Sơn

**Files:**
- Create: `desktop/bridge.c`, `desktop/bridge.h`
- Create: `desktop/main.c`
- Create: `tests/test_bridge.c`
- Create: `tests/fixtures/spike_test.html`
- Modify: `Makefile`

**Interfaces:**
- Consumes:
  - `desktop/tcp_client.h`: `Client`, `client_connect`, `client_send`, `client_poll`, `client_close`
  - `shared/protocol.h`: `Envelope`, `frame_encode`, `Status`
- Produces:
  - `Status bridge_init(WebKitWebView *web_view, Client *client)`
  - `Status bridge_dispatch(Client *client, const char *json_str, size_t len)`
  - `void bridge_emit(WebKitWebView *web_view, const Envelope *e)`
  - Target `make desktop-smoke`: khởi chạy server port ngẫu nhiên, mở GTK WebKit load fixture `spike_test.html`, gửi `HELLO` và thoát exit code 0 khi nhận `OK`.

- [x] **Step 1: Viết test failing cho bridge parser và custom scheme security**

Trong `tests/test_bridge.c`:
- `test_bridge_dispatch_validation`: truyền JSON không hợp lệ, JSON không đúng format Envelope, type không nằm trong whitelist -> xác nhận trả về `BAD_REQUEST`.
- `test_scheme_path_traversal`: kiểm tra hàm phân giải URI của `uno://app` chặn các URI chứa `../`, `..%2f`, các ký tự escape hoặc đường dẫn tuyệt đối ra ngoài thư mục assets.

Trong `tests/fixtures/spike_test.html`:
- Chứa kịch bản JavaScript:
  ```javascript
  window.addEventListener('uno:message', (event) => {
    if (event.detail && event.detail.type === 'OK') {
      window.webkit.messageHandlers.uno.postMessage(JSON.stringify({v: 1, id: 999, type: 'SPIKE_DONE', payload: {}}));
    }
  });
  window.webkit.messageHandlers.uno.postMessage(JSON.stringify({v: 1, id: 1, type: 'HELLO', payload: {}}));
  ```

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `make test NAME=bridge`
Expected: Fail do chưa cài đặt `bridge.c`.

- [x] **Step 3: Cài đặt WebKitGTK Custom URI Scheme và Bridge C**

- Cập nhật `Makefile`: thêm cờ biên dịch `$(shell pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1)`.
- Triển khai `desktop/bridge.c`, `desktop/bridge.h`:
  - Đăng ký URI scheme `uno://app` thông qua `webkit_web_context_register_uri_scheme()`. Hàm callback đọc file tĩnh từ thư mục assets tương ứng với MIME type (HTML, JS, CSS, PNG). Chặn chặt chẽ path traversal (`strstr(path, "..")`).
  - Đăng ký Script Message Handler `"uno"` trên `WebKitUserContentManager`. Nhận chuỗi JSON từ JS, gọi `bridge_dispatch()`, parse thành `Envelope` và gọi `client_send()`.
  - `bridge_emit()`: serialize `Envelope` thành chuỗi JSON an toàn bằng `json-object-to-json-string()`, tạo đoạn mã gọi `window.dispatchEvent(new CustomEvent('uno:message', {detail: %s}))` thông qua `webkit_web_view_evaluate_javascript()`.
- Triển khai `desktop/main.c`:
  - Khởi tạo GTK window và `WebKitWebView`.
  - Tích hợp socket fd của `Client` vào GLib event loop bằng `g_io_add_watch()`, khi có dữ liệu gọi `conn_read` -> `bridge_emit`.

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `make test NAME=bridge`
Expected: Output `bridge: OK`, exit code 0.
Run: `xvfb-run -a make desktop-smoke`
Expected: GTK webview khởi động ngầm, fixture `spike_test.html` gửi `HELLO`, nhận `OK` từ server TCP C, bridge nhận `SPIKE_DONE` và thoát tiến trình với exit code 0.

- [x] **Step 5: Báo cáo kết quả Spike**

Ghi nhận bằng chứng kiểm thử thông tuyến: JS Webview <-> C Bridge <-> POSIX TCP <-> C Server. Khẳng định kiến trúc khả thi 100% trước khi bước vào Task 3.

---

### Task 3: SQLite và transaction trận — Sơn

**Files:**
- Create: `server/schema.sql`
- Create: `server/store.h`, `server/store.c`
- Create: `tests/test_store.c`
- Modify: `server/app.c`, `Makefile`

**Interfaces:**
- Consumes:
  - `shared/contracts.h`: `Status`, `MatchId`, `PlayerSpec`, `Mode`, `EventBatch`, `MatchResult`, `ProfileDelta`, `UserId`
- Produces:
  - `Status store_open(Store **out, const char *path)`
  - `void store_close(Store *s)`
  - `Status store_match_start(Store *s, const MatchId *match_id, const PlayerSpec players[4], Mode mode, bool ranked)`
  - `Status store_events(Store *s, const MatchId *match_id, const EventBatch *batch)`
  - `Status store_finalize(Store *s, const MatchResult *result, const ProfileDelta deltas[4])`
  - `Status store_export_match(Store *s, const MatchId *match_id, const char *path)`

- [x] **Step 1: Viết test failing `tests/test_store.c`**

Trong `tests/test_store.c`:
- Mở cơ sở dữ liệu tạm thời `:memory:` hoặc file temp `/tmp/uno_test_store.db`.
- `test_store_match_start_and_events`: bắt đầu trận, lưu các batch event với sequence tăng dần; xác nhận truy vấn DB thấy đúng dữ liệu.
- `test_event_seq_unique`: chèn 2 event có cùng `(match_id, seq)` -> xác nhận phát hiện xung đột và trả về `INVALID_ACTION`.
- `test_store_finalize_idempotence`: gọi `store_finalize` lần 1 thành công; gọi lại lần 2 với cùng dữ liệu -> trả về `OK` và không nhân đôi thống kê; gọi lại lần 2 với dữ liệu sai khác -> trả về `INVALID_ACTION`.
- `test_crash_recovery_aborted`: tạo một match với status `MATCH_ACTIVE`. Đóng store, mở lại store -> xác nhận hàm khởi tạo tự động cập nhật status của match dở dang đó thành `MATCH_ABORTED`.
- `test_transaction_rollback_on_failure`: tạo trigger SQL tạm thời bắn lỗi `RAISE(ABORT)` giữa transaction finalize -> xác nhận toàn bộ thay đổi bị rollback, số dư/thống kê giữ nguyên.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `make test NAME=store`
Expected: Biên dịch lỗi do thiếu `server/store.h` hoặc test runner fail.

- [x] **Step 3: Cài đặt schema SQL và store repository**

- Cập nhật `Makefile`: thêm `$(shell pkg-config --cflags --libs sqlite3)`.
- Triển khai `server/schema.sql`:
  - `PRAGMA foreign_keys = ON;`
  - Bảng `matches`: `match_id TEXT PRIMARY KEY, mode INTEGER, ranked INTEGER, status INTEGER, winner_seat INTEGER, winner_team INTEGER, duration_ms INTEGER, created_at INTEGER`.
  - Bảng `match_players`: `match_id TEXT, seat INTEGER, user_id INTEGER, team INTEGER, points INTEGER, elo_before INTEGER, elo_after INTEGER, PRIMARY KEY (match_id, seat)`.
  - Bảng `match_events`: `match_id TEXT, seq INTEGER, revision INTEGER, actor INTEGER, type TEXT, payload TEXT, PRIMARY KEY (match_id, seq)`.
  - Bảng `accounts`, `friends`, `achievements` chuẩn bị cho Task 4 & 11.
- Triển khai `server/store.c`, `server/store.h`:
  - `store_open`: mở SQLite với cờ `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX`. Đặt `PRAGMA busy_timeout = 100;`, `PRAGMA journal_mode = WAL;`. Thực thi migration `schema.sql`. Quét các trận `MATCH_ACTIVE` cập nhật thành `MATCH_ABORTED`. Chuẩn bị sẵn các prepared statements.
  - `store_events`: bọc trong transaction `BEGIN IMMEDIATE ... COMMIT`.
  - `store_finalize`: kiểm tra trạng thái trận hiện tại. Nếu đã `MATCH_FINALIZED` với cùng kết quả -> trả về `OK`. Nếu chưa finalized -> thực hiện ghi `match_players`, cập nhật `matches.status = MATCH_FINALIZED`, cập nhật `achievements` trong một transaction duy nhất.
  - `store_export_match`: đọc toàn bộ match, players và ordered events, xuất ra file JSON theo format chuẩn.

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `make test NAME=store`
Expected: Output `store: OK`, exit code 0.

- [x] **Step 5: Kiểm tra ASan/UBSan**

Run: `make sanitize NAME=store`
Expected: Clean memory leak và không lỗi handle SQLite.

---

### Task 4: Account, session và auth worker — Sơn

**Files:**
- Create: `server/auth_worker.h`, `server/auth_worker.c`
- Create: `server/accounts.h`, `server/accounts.c`
- Create: `tests/test_accounts.c`
- Create: `tests/integration/test_auth.py`
- Modify: `server/store.h`, `server/store.c`, `server/app.c`, `Makefile`

**Interfaces:**
- Consumes:
  - `shared/protocol.h`: `Envelope`, `Status`
  - `shared/contracts.h`: `ConnId`, `UserId`
  - `server/store.h`: DB connection
- Produces:
  - Store additions:
    - `typedef struct { char email[255]; char hash[128]; char name[65]; char avatar[32]; } AccountCreate;`
    - `typedef struct { UserId id; char email[255]; char hash[128]; char name[65]; char avatar[32]; int elo; } Account;`
    - `Status store_account_create(Store *s, const AccountCreate *c, UserId *out_id)`
    - `Status store_account_get(Store *s, const char *email, Account *out)`
    - `Status store_account_get_by_id(Store *s, UserId id, Account *out)`
  - Auth Worker:
    - `typedef enum { AUTH_REGISTER, AUTH_LOGIN } AuthJobKind;`
    - `typedef struct { AuthJobKind kind; ConnId conn_id; int32_t req_id; char email[255]; char password[129]; char name[65]; char avatar[32]; } AuthJob;`
    - `typedef struct { ConnId conn_id; int32_t req_id; Status status; UserId user_id; char token[65]; Account account; } AuthResult;`
    - `Status auth_worker_init(AuthWorker **out, int notify_pipe_fd)`
    - `Status auth_submit(AuthWorker *w, const AuthJob *job)`
    - `void auth_worker_destroy(AuthWorker *w)`
  - Accounts & Sessions:
    - `Status accounts_handle(App *app, ConnId conn_id, const Envelope *e)`
    - `Status sessions_user(App *app, ConnId conn_id, UserId *out)`
    - `Status sessions_resume(App *app, ConnId conn_id, const char *token)`
    - `void sessions_disconnect(App *app, ConnId conn_id)`
    - `void auth_drain(App *app)`

- [x] **Step 1: Viết test failing `tests/test_accounts.c` và `tests/integration/test_auth.py`**

Trong `tests/test_accounts.c`:
- `test_account_validation`: email hợp lệ vs không hợp lệ (hoa thường, thiếu `@`, quá 254 bytes); mật khẩu < 6 bytes và > 128 bytes bị từ chối; tên UTF-8 tiếng Việt 16 codepoints hợp lệ, 17 codepoints bị từ chối; avatar nằm ngoài whitelist bị từ chối.
- `test_token_generation`: sinh token 32 byte ngẫu nhiên, mã hóa chuỗi hex 64 ký tự, kiểm tra tính duy nhất.

Trong `tests/integration/test_auth.py`:
- `test_register_and_login_flow`: đăng ký tài khoản thành công -> đăng nhập đúng mật khẩu trả token và `me` -> đăng nhập sai mật khẩu trả `UNAUTHENTICATED`.
- `test_rate_limiting`: gửi 6 request auth từ cùng một IP mà không có cờ nới lỏng -> request thứ 6 trả về `RATE_LIMITED`.
- `test_auth_worker_nonblocking`: gửi liên tiếp các job auth, đồng thời từ connection khác gửi `PING` -> xác nhận `PING` nhận response dưới 500ms mà không bị nghẽn bởi tác vụ hash.
- `test_fd_reuse_after_disconnect`: client gửi LOGIN rồi đóng kết nối ngay lập tức; slot kết nối bị tái sử dụng với generation mới -> xác nhận kết quả hash cũ từ worker bị hủy an toàn, không gán nhầm session cho connection mới.

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `make test NAME=accounts` và `make integration NAME=auth`
Expected: Fail do chưa có `auth_worker` và `accounts`.

- [x] **Step 3: Cài đặt Libsodium Auth Worker và Session Manager**

- Cập nhật `Makefile`: thêm `$(shell pkg-config --cflags --libs libsodium)`.
- Triển khai `server/auth_worker.c`, `server/auth_worker.h`:
  - Khởi tạo 1 pthread worker đọc queue có kích thước cố định 8 phần tử, đồng bộ bằng `pthread_mutex_t` và `pthread_cond_t`.
  - Xử lý hash bằng `crypto_pwhash_str` và verify bằng `crypto_pwhash_str_verify` với `crypto_pwhash_OPSLIMIT_INTERACTIVE` và `crypto_pwhash_MEMLIMIT_INTERACTIVE`.
  - Xóa sạch bộ nhớ mật khẩu thô ngay sau khi sử dụng bằng `sodium_memzero()`.
  - Báo kết quả về poll loop bằng cách ghi `AuthResult` vào non-blocking pipe.
- Triển khai `server/accounts.c`, `server/accounts.h`:
  - Quản lý mảng session ánh xạ `ConnId` <-> `UserId` và token 64 hex.
  - Bảng rate limit IP trượt (sliding window 1 phút, tối đa 5 auth requests/IP, cấu hình được bằng cờ `--auth-limit`).
  - `auth_drain()`: đọc từ pipe, đối chiếu `conn_id.slot` và `conn_id.generation` của connection hiện tại trên server. Nếu generation không khớp hoặc socket đã đóng, drop kết quả an toàn. Nếu khớp, trả response `OK` kèm token cho client.
  - Xử lý `RESUME` token, `LOGOUT`, và thu hồi token cũ khi có lượt đăng nhập mới của cùng một tài khoản.

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `make test NAME=accounts`
Expected: Output `accounts: OK`, exit code 0.
Run: `make integration NAME=auth`
Expected: Tất cả các case register, login, rate-limit, fd-reuse pass.

- [x] **Step 5: Kiểm tra regression**

Run: `make integration NAME=transport`
Expected: Không bị ảnh hưởng, toàn bộ test transport vẫn pass.

---

### Task 5: Phòng và party/invite có quyền host — Sơn

**Files:**
- Create: `server/rooms.h`, `server/rooms.c`
- Create: `tests/test_rooms.c`
- Create: `tests/integration/test_rooms.py`
- Modify: `server/app.c`, `Makefile`

**Interfaces:**
- Consumes:
  - `shared/protocol.h`: `Envelope`, `Status`
  - `shared/contracts.h`: `ConnId`, `UserId`, `Seat`, `Mode`, `BotLevel`, `PlayerSpec`
  - `server/accounts.h`: `sessions_user`
- Produces:
  - `typedef struct { char roomId[7]; Mode mode; bool ranked; PlayerSpec players[4]; } RoomStart;`
  - `typedef Status (*OnMatchStart)(App *app, const RoomStart *rs, MatchId *out_match_id);`
  - `Status rooms_init(Rooms **out, App *app, OnMatchStart on_start)`
  - `void rooms_destroy(Rooms *r)`
  - `Status rooms_handle(App *app, ConnId conn_id, const Envelope *e)`
  - `Status rooms_start(App *app, const char *room_id)`
  - `void rooms_disconnect(App *app, UserId user_id, uint64_t now_ms)`
  - `Status rooms_make_match(App *app, Mode mode, const UserId users[4], MatchId *out_match_id)`

- [x] **Step 1: Viết test failing `tests/test_rooms.c` và `tests/integration/test_rooms.py`**

Trong `tests/test_rooms.c`:
- `test_room_code_generation`: sinh mã phòng đúng 6 ký tự in hoa `[A-Z0-9]`, kiểm tra cơ chế retry khi trùng mã.
- `test_room_capacity_and_join`: tối đa 16 phòng đồng thời; người thứ 5 join phòng nhận `ROOM_FULL`.
- `test_host_privileges`: đổi mode (`ROOM_MODE`), thêm/sửa bot (`ROOM_BOT`), kick (`ROOM_KICK`), bắt đầu (`ROOM_START`) từ người không phải host -> nhận `FORBIDDEN`.
- `test_host_migration`: host rời phòng trước khi bắt đầu -> quyền host tự động chuyển giao cho human còn lại có số ghế (seat) nhỏ nhất. Khi toàn bộ human rời -> phòng bị hủy.
- `test_bot_configuration`: kiểm tra thêm bot với đủ 3 mức `EASY`, `NORMAL`, `HARD`.

Trong `tests/integration/test_rooms.py`:
- `test_room_state_broadcast`: khi có người vào/rời phòng, các thành viên khác nhận được push `ROOM_STATE` chứa đúng revision và danh sách 4 ghế (không lộ token hay email).
- `test_invite_and_expiry`: gửi `INVITE_SEND` sang user khác; kiểm tra sau 30s không phản hồi thì invite tự hết hạn; người nhận đang bận hoặc phòng đã đầy thì từ chối hợp lệ.
- `test_party_limits`: party chỉ cho phép tối đa 2 người và chỉ áp dụng cho mode team (`TWO_V_TWO`, `SIDE`).

- [x] **Step 2: Chạy test kiểm tra trạng thái đỏ (fail)**

Run: `make test NAME=rooms` và `make integration NAME=rooms`
Expected: Fail do chưa có `rooms.c`.

- [x] **Step 3: Cài đặt quản lý phòng và party**

- Triển khai `server/rooms.c`, `server/rooms.h`:
  - Cấu trúc `Room`: `char code[7]; Mode mode; bool ranked; Seat host_seat; PlayerSpec seats[4]; uint64_t revision;`.
  - Quản lý cố định 16 slot phòng. Sinh mã phòng ngẫu nhiên 6 ký tự bằng `randombytes_buf`.
  - Xử lý các request:
    - `ROOM_CREATE`: tạo phòng mới, gán người tạo làm host ở ghế 0.
    - `ROOM_JOIN`: tìm phòng theo mã code, tìm ghế trống nhỏ nhất, phát `ROOM_STATE`.
    - `ROOM_LEAVE`: rời phòng, xử lý di chuyển host hoặc hủy phòng.
    - `ROOM_BOT`: host thêm/sửa bot tại ghế chỉ định.
    - `ROOM_KICK`: host đuổi người ở ghế chỉ định.
    - `ROOM_START`: kiểm tra 4 ghế đã đầy và không có ai đang bận; gọi callback `on_start` (chuẩn bị nối Task 9). Nếu start thất bại thì giữ nguyên phòng, không đổi trạng thái.
  - Quản lý lời mời (Invite) và Party (tối đa 2 người, lưu trong session).

- [x] **Step 4: Chạy test kiểm tra trạng thái xanh (pass)**

Run: `make test NAME=rooms`
Expected: Output `rooms: OK`, exit code 0.
Run: `make integration NAME=rooms`
Expected: Toàn bộ integration test phòng và invite pass.

- [x] **Step 5: Kiểm tra ASan/UBSan**

Run: `make sanitize NAME=rooms`
Expected: Không có cảnh báo bộ nhớ.

---

## Tự rà soát bản plan (Self-Review Checklist)

1. **Spec Coverage:**
   - Task 2 bao quát: POSIX TCP socket, nonblocking, `poll()`, 64 clients, 256 KiB buffer limit, framing spec.
   - Task 2.5 (Spike 12) bao quát: GTK3 + WebKitGTK 4.1 URI scheme `uno://app`, an toàn path traversal, bridge 2 chiều JS <-> C native, thông tuyến HELLO qua TCP.
   - Task 3 bao quát: SQLite WAL, `schema.sql`, prepared statements, idempotency của finalize/events, crash recovery active->aborted, transaction rollback.
   - Task 4 bao quát: Libsodium password hash/verify trong pthread worker (queue 8), nonblocking pipe notification, tránh race condition fd-reuse nhờ `generation`, validation email/password/name UTF-8, rate-limit 5/phút/IP.
   - Task 5 bao quát: 16 phòng, 6-char uppercase code, quyền host, di chuyển host, bot 3 mức, invite 30s timeout, party 2 người.
2. **Step Scan:** Mỗi task chia nhỏ thành 5 bước rõ ràng: Viết failing test -> Chạy đỏ -> Cài đặt tối thiểu -> Chạy xanh -> Kiểm tra sanitize / integration.
3. **Type Consistency:** Toàn bộ kiểu dữ liệu tuân thủ nghiêm ngặt `shared/contracts.h` (`Status`, `UserId`, `Seat`, `ConnId`, `Mode`, `BotLevel`, `MatchResult`, `ProfileDelta`) và `shared/protocol.h` (`Envelope`, `FrameReader`).
4. **Review Focus:** 5 failure modes quan trọng nhất đều có test cụ thể được ghim vào từng task tương ứng.
5. **Proportion:** Ngắn gọn, súc tích, tập trung vào contracts, signatures và test assertions, không viết code thừa.
