# UNO Ubuntu TCP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hoàn thành UNO multiplayer trên Ubuntu qua TCP C, giữ UI React desktop và đầy đủ các chức năng Bách–Sơn.

**Architecture:** React hiển thị trong GTK/WebKitGTK, trao đổi nội bộ với native C; native kết nối TCP C server bằng length-prefix + JSON. Server sở hữu state, timer và xác thực; engine thuần C không phụ thuộc socket/DB. Sơn làm mạng, sảnh và persistence; Bách làm game, hồ sơ/thành tựu và toàn bộ UI.

**Tech Stack:** Ubuntu 24.04 LTS amd64; C11/GCC/Make/poll/pthreads; GTK3/WebKitGTK 4.1; json-c, SQLite3, libsodium; React18/Vite5/Vitest2 hiện có; Node22; Python3 unittest cho integration.

**Spec:** [2026-11-11-ubuntu-tcp-design.md](../specs/2026-11-11-ubuntu-tcp-design.md). Đọc cả hai tài liệu trước khi thực thi. Không dùng ARCHITECTURE.md cũ làm spec cho transport.

## Mô hình I/O sử dụng

**Chọn: I/O multiplexing bằng `poll()` kết hợp nonblocking I/O**, một thread cho toàn bộ socket.

Lý do xuất phát từ đặc điểm của chính project:

1. **State dùng chung giữa các kết nối.** 4 người một phòng cùng sửa một `Game`; phòng, hàng chờ, session, presence đều liên quan nhiều kết nối. Một thread xử lý lần lượt từng sự kiện thì không cần mutex, không có race. Mô hình mỗi kết nối một thread (blocking I/O) buộc khóa mọi cấu trúc này.
2. **Thứ tự sự kiện phải xác định.** Hết giờ lượt và lệnh PLAY đến cùng lúc thì chỉ một bên thắng, đúng một lần (Review Focus 4). Một vòng lặp cho phép quy định cứng "tick timer trước, đọc input sau". Nhiều thread hoặc signal handler không đảm bảo được thứ tự này.
3. **Server cần timer.** Lượt 20s, UNO, heartbeat, bot delay. Tham số timeout của `poll()` chính là đồng hồ: ngủ đến deadline gần nhất hoặc đến khi có dữ liệu. Không cần thread timer hay signal.
4. **Một client chậm không được làm treo người khác** (Review Focus 1). Socket nonblocking: `recv`/`send` không đủ dữ liệu/chỗ thì trả `EAGAIN` ngay, server giữ phần dở trong buffer của kết nối đó và phục vụ kết nối khác.
5. **Quy mô nhỏ.** Tối đa 64 kết nối, 16 phòng: một thread dư sức, không cần mô hình phức tạp hơn.
6. **Dễ kiểm thử.** Một thread + đồng hồ truyền vào (`now_ms`) thì test timeout chạy lặp lại được, không phụ thuộc lịch chạy thread.

Hai mô hình phải đi cùng nhau: nonblocking một mình thành busy-loop đốt CPU; `poll()` với socket blocking vẫn có thể treo ở `send` khi buffer đầy hoặc ở `accept` khi client hủy kết nối giữa chừng.

**Vì sao `poll`, không `select` hay `epoll`:**

- So với `select`: mảng `pollfd` khớp thẳng với mảng slot kết nối của server, không phải dựng lại `fd_set` và tính `maxfd+1` mỗi vòng, không bị trần `FD_SETSIZE`, tách riêng `events`/`revents` nên phân biệt rõ `POLLHUP`/`POLLERR`. Cùng nhóm với `select` trong bài giảng.
- So với `epoll`: chỉ có lợi khi hàng nghìn fd. Với 64 fd, quét tuyến tính không đáng kể; `epoll` thêm code đăng ký/sửa/gỡ fd mà không đổi lại gì.

Đối chiếu 5 mô hình trong bài giảng:

| Mô hình | Dùng? | Ở đâu / vì sao |
|---|---|---|
| Blocking I/O | Không cho socket | Một client chậm sẽ treo cả server. Chỉ còn chặn ở 2 chỗ không phải socket: SQLite ghi đĩa trên main loop (ngắn, `busy_timeout` 100ms) và worker hash mật khẩu chờ job trong thread riêng |
| Nonblocking I/O | **Có** | Mọi socket đặt `O_NONBLOCK`: listener `accept` đến `EAGAIN`, `recv`/`send` xử lý `EAGAIN`/`EINTR` và partial read/write, client `connect` nonblocking rồi kiểm tra `SO_ERROR`. Không dùng một mình (không busy-loop), luôn đi kèm `poll()` — Task 2 |
| I/O multiplexing | **Có — mô hình chính** | Server: một thread, một vòng `poll()` trên listener + tối đa 64 connection + pipe của auth worker; timeout của `poll()` dùng cho timer lượt/UNO/heartbeat — Task 2, 4, 9. Client desktop: GLib main loop (bên trong cũng là `poll()`) theo dõi fd socket qua fd source, nhờ đó mạng không chặn giao diện — Task 12 |
| Signal-driven I/O (SIGIO) | Không | Với TCP, `SIGIO` bắn cho quá nhiều loại sự kiện (có dữ liệu, kết nối xong, đối phương đóng, buffer gửi trống, lỗi) mà không cho biết là sự kiện nào trên fd nào, nên vẫn phải `poll` lại. Handler chạy chen ngang bất kỳ lúc nào, chỉ được gọi hàm async-signal-safe, phá yêu cầu thứ tự ở lý do 2. Chỉ xử lý signal để tắt server sạch (`SIGINT`/`SIGTERM`) và bỏ qua `SIGPIPE` |
| Asynchronous I/O (POSIX `aio_*`) | Không | Trên Linux, glibc cài `aio_*` bằng thread ngầm, thiết kế cho file, không hợp socket. Kết quả trả về qua signal hoặc callback ở thread khác, tức đưa lại đúng vấn đề đồng thời mà lý do 1–2 muốn tránh. Việc nặng duy nhất (hash mật khẩu) chạy ở một worker pthread, báo kết quả về vòng `poll()` qua pipe — Task 4 |

## Thứ tự làm và điểm phải chờ nhau

Hai người làm song song gần như toàn bộ. Chỉ có **4 điểm chờ** (in đậm trong bảng).

| Bước | Bách | Sơn |
|---|---|---|
| 1 | Task 1 (code) | **Chờ Task 1**, rồi review framing + Makefile. Trong lúc chờ: cài môi trường Ubuntu, đọc plan |
| 2 | Task 6 → 7 → 8. Không chờ Sơn | Task 2 → spike Task 12 → Task 3 → 4 → 5. Không chờ Bách |
| 3 | **Task 9 chờ Sơn xong Task 3, 4, 5.** Nếu Sơn chưa xong: làm trước phần test + adapter của Task 13 bằng fixture `views.json` (Task 8) | Xong Task 5 thì nối dispatch Task 9 cùng Bách. Trong lúc chờ Bách: làm nốt Task 12, phần social của Task 11 |
| 4 | Task 13 (chạy thật cần Task 9 + Task 12 của Sơn) | **Task 10 chờ Bách xong Task 9** |
| 5 | **Phần profile Task 11 chờ Sơn xong Task 10**, rồi Task 14 | Hỗ trợ sửa `store.c` cho Task 11 |
| 6 | Task 15 cùng làm | Task 15 cùng làm |

Phụ thuộc từng task (task bên phải phải xanh trước):

| Task | Người làm | Cần xong trước | Chờ người kia? |
|---|---|---|---|
| 1 | Bách | — | Không |
| 2 | Sơn | 1 | **Có — chờ Bách** |
| 12 spike | Sơn | 2 | Không |
| 3 → 4 → 5 | Sơn | 2, theo thứ tự | Không |
| 6 → 7 → 8 | Bách | 1, theo thứ tự | Không |
| 9 | Bách (Sơn nối dispatch) | 8 và 3, 4, 5 | **Có — chờ Sơn** |
| 10 | Sơn | 9 | **Có — chờ Bách** |
| 11 social | Sơn | 4 | Không |
| 11 profile | Bách | 10 | **Có — chờ Sơn** |
| 12 phần còn lại | Sơn | 2 | Không |
| 13 | Bách | 8 (unit test); 9 + 12 (chạy thật) | Chỉ khi chạy thật |
| 14 | Bách | 13; 4, 5, 10, 11 (chạy thật) | Chỉ khi chạy thật |
| 15 | Cả hai | Tất cả | — |

Quy tắc file chung: sau Task 1, chỉ Sơn sửa `shared/*`, `Makefile`, `server/store.*`, `server/app.*`; chỉ Bách sửa `server/{game,bot,view,match,profile}.*` và `client/src/**`. Cần đổi file của người kia thì nhờ người đó sửa.

Spike Task 12 (bắt buộc, ngay sau Task 2): WebKitGTK load `client/dist` qua `uno://app` và HELLO đi hết đường JS→bridge→TCP→server→JS. Đây là rủi ro lớn nhất chưa ai chạy thử; nếu không đạt, báo bằng evidence trước khi làm tiếp Task 3.

## Global Constraints

- Chỉ chạy Ubuntu; server và client mạng viết bằng C dùng POSIX TCP socket; không WebSocket.
- Chỉ tạo tài liệu trong lượt này, **không commit, không tạo/chuyển/xóa branch, không triển khai code**.
- Không có bước commit trong plan theo yêu cầu người dùng; chỉ thêm bước đó khi được cho phép sau.
- Mọi giá trị về luật, timeout, giới hạn và security theo spec §§2–6; không tùy ý đổi sang luật UNO khác.
- Server-authoritative; không chuyển engine/timer/ELO lại về client để hoàn thiện demo nhanh.
- Một DB do Sơn sở hữu; một UI do Bách sở hữu. Không để cả hai sửa cùng file build/protocol/schema.
- Mỗi task viết test trước, chạy thấy lỗi đúng nguyên nhân, viết code tối thiểu, chạy pass rồi mới tiếp tục. Không coi lỗi thiếu compiler là test logic đã fail.
- Lệnh dưới đây chạy từ repository root trong Ubuntu. Không cài dependency hay chạy lệnh triển khai trong lượt lập plan này.
- `make test NAME=x` là contract build: chạy đúng executable `build/tests/test_x`; integration `make integration NAME=x` chạy `python3 -m unittest tests.integration.test_x -v`. Task 1 cung cấp quy ước này; task sau bổ sung target/test file của mình.

## Review Focus

1. TCP bị chia/dính gói, partial send và peer chậm: không mất request/không chặn các phòng khác — Task 1–2.
2. User giả actor, gửi lại command hoặc dùng ghế của người khác: server từ chối, không thay state — Task 4, 7, 9.
3. Snapshot dành cho ghế 1/2/3 hoặc bot hard: không lộ bài đối thủ, UI xoay đúng đội — Task 8, 13.
4. Timeout/UNO/disconnect đúng lúc action đến: xử lý đúng một lần, callback cũ không tác động ván mới — Task 9.
5. Lỗi DB/crash giữa chốt trận hoặc hash trả về connection đã đóng: không double ELO/thành tựu, không đăng nhập nhầm người — Task 3–4, 10–11.

---

## Cấu trúc file và contract dùng chung

Tạo mới (chưa tồn tại):

| File | Trách nhiệm |
|---|---|
| Makefile, .gitignore | build riêng server/native/tests; bỏ qua build/, runtime DB và export log |
| shared/contracts.h | chỉ kiểu mà cả hai người cùng dùng: Status, ID, Mode, PlayerSpec, EventBatch, MatchResult, ProfileDelta; không chứa state global, không chứa model nội bộ engine |
| shared/protocol.c/.h, shared/net.c/.h | JSON envelope/framing và non-blocking buffers/socket |
| server/main.c, app.c/.h | poll loop, dispatch, session map, wiring module |
| server/store.c/.h, schema.sql | toàn bộ SQL/transaction; API repository |
| server/accounts.c/.h, auth_worker.c/.h | validation account, session, worker hash |
| server/social.c/.h, rooms.c/.h, queue.c/.h | bạn bè/presence, phòng/party/invite, matchmaking |
| server/game.c/.h, bot.c/.h | engine pure C (game.h định nghĩa Card/Intent/Game) và quyết định bot |
| server/match.c/.h, view.c/.h | lifecycle/timer match và snapshot riêng theo ghế |
| server/profile.c/.h | update hồ sơ, tính progress từ log qua store API |
| desktop/main.c, bridge.c/.h, tcp_client.c/.h | GTK host, bridge nội bộ và POSIX TCP client |
| client/src/net/{bridge.js,session.jsx,useRemoteGame.js,view.js} | API UI bất đồng bộ, state server, chuyển view cho component hiện có |
| tests/test_*.c, tests/integration/{__init__.py,helpers.py,test_*.py} | unit C và integration qua TCP; không dùng mock server để kết luận multiplayer thật |
| tests/fixtures/{requests.json,views.json}, docs/protocol.md, docs/ubuntu-demo.md | contract fixtures, wire spec, hướng dẫn demo |

Sửa các file UI cụ thể tại Task 12–14. Không xóa các test engine JS hiện có; giữ làm tham chiếu regression trong quá trình port.

### Model cần định nghĩa trong Task 1 (`shared/contracts.h`)

Chỉ kiểu đi qua ranh giới Bách–Sơn. `Status` enum gồm OK và các mã lỗi ở spec §3 (tên lỗi lưu trữ thống nhất là `STORAGE_ERROR`); các API trả Status, không trả chuỗi lỗi tự do. `UserId` = uint64_t (0 dành bot/empty); `Seat` = uint8_t 0..3; `Mode` = CLASSIC/TWO_V_TWO/SIDE/DRAFT; `BotLevel` = EASY/NORMAL/HARD. `MatchId` = UUID string 36 ký tự + NUL. `ConnId` = `{uint32_t slot, generation;}`. `PlayerSpec` = userId/name/avatar/botLevel/isBot. `EventBatch` tối đa 32 event/action với revision/actor/type/payload chuẩn hóa, đủ tái dựng lịch sử (một event có thể chứa danh sách lá). `MatchResult` gồm matchId/mode/ranked/status/winnerSeat/winnerTeam/duration và players[4] (userId/team/left/points/stats/eloBefore/eloAfter). `ProfileDelta` gồm counters, streak/bestStreak/peakElo, badge progress. `Store`, `App`, `Rooms`, `Queue`, `Matches`, `Client` là opaque structs định nghĩa bởi task sở hữu.

### Model nội bộ engine — không thuộc Task 1

Chỉ module của Bách dùng nên định nghĩa tại task sở hữu, sửa không cần qua Sơn. Task 6 (`server/game.h`): `Card` = id string c1..c112/color/value; `Intent` = kind PLAY/DRAW/PASS/SAY_UNO/DRAFT_PICK/LEAVE/CHAT, matchId/turnNo/cardId/color/draftTurn/index/chatId (actor là tham số riêng từ session); `Game` chứa deck/discard/hands 112 lá tổng cộng, player specs[4], mode/team/turn/turnNo/direction, phase, draft state, stats, winner; không chứa socket/DB. Task 8 (`server/view.h`): `GameView` theo spec §4.

## Thứ tự và mốc tích hợp

- Thứ tự và điểm chờ: xem mục "Thứ tự làm và điểm phải chờ nhau" ở đầu plan.
- Task 1 do Bách code, Sơn review API framing (`frame_feed`/`frame_encode`) và Makefile trước khi bắt đầu Task 2. Sau khi Task 1 xanh và được duyệt, `shared/protocol.*`, `shared/contracts.h`, Makefile giao lại Sơn giữ.
- Task 9 nối hai mảng và phải chơi được qua headless TCP trước khi làm đẹp GUI.
- Task 15 là kiểm chứng cuối trên Ubuntu thật/LAN; không bỏ chức năng account/social để đổi lấy demo game.
- Mốc review: contract; TCP+auth; engine+view; một ván TCP; persistence/social; desktop/LAN. Dừng sửa interface chung tùy tiện sau mốc contract: cập nhật docs/fixture/caller/tests cùng một thay đổi.

### Task 1: Gói tin chung và build kiểm thử — Bách code, Sơn review

**Chuẩn bị:** Ubuntu 24.04 (WSL2 đủ cho task này) cần `build-essential pkg-config libjson-c-dev`.

**Files:** Create `Makefile`, `shared/contracts.h`, `shared/protocol.c`, `shared/protocol.h`, `tests/test_protocol.c`, `tests/fixtures/requests.json`, `docs/protocol.md`; Modify `.gitignore`.

**Interfaces:** Produces `Status frame_feed(FrameReader *r, const uint8_t *data, size_t len, FrameCallback cb, void *ctx)`; callback `Status (*)(const Envelope *, void *)`; `Status frame_encode(const Envelope *e, uint8_t *out, size_t cap, size_t *written)`; `void frame_reader_reset(FrameReader *r)`. FrameReader sở hữu buffer 65540 byte; Envelope={v,id,type,payload(json_object*)}, callback mượn object chỉ trong call; encode không nhận ownership. C API model theo mục "Model cần định nghĩa trong Task 1", JSON field theo spec §3. Task này chỉ làm framing/envelope, validation command do module sở hữu.

- [x] **1. Viết test** `test_fragmented_and_coalesced`, `test_frame_limits`, `test_invalid_json`: encode HELLO, feed 1 byte/lần rồi hai frame nối nhau; `assert(callbacks == 3)`; `assert(reject_length(0)); assert(reject_length(65537)); assert(reject_trailing_bytes()); assert(reject_depth(17));` thêm UTF-8 hỏng/NUL và payload không phải object. Helper reject_* là hàm static trong chính test, gọi frame_feed.
- [x] **2. Chạy đỏ** `make test NAME=protocol`: executable phải build được runner rồi fail assertion/thiếu implementation framing; ghi rõ lỗi. Khởi tạo target test tối thiểu và khai báo header cùng test trước implementation.
- [x] **3. Cài đặt** protocol + model/header trên, encode big-endian từng byte, json-c parse strict bằng tokener có sẵn (giới hạn depth 16, validate UTF-8, không trailing; kiểm tra offset parse == length). Key trùng không chặn riêng: json-c giữ giá trị cuối, không tự viết bộ quét JSON. Fixtures gồm request/OK/ERROR/push và boundary values. Build dùng `-std=c11 -Wall -Wextra -Wpedantic -Werror`, target `all`, `server`, `desktop`, `test`, `integration`, `sanitize`, `package`. Nguồn lấy bằng wildcard `shared/*.c` + `server/*.c` (trừ `server/main.c`), pattern rule `build/tests/test_%` từ `tests/test_%.c`: thêm module/test mới không phải sửa Makefile, module chưa có tự không tham gia link. Chỉ thêm thư viện mới (sqlite3, libsodium, GTK) mới cần sửa Makefile, do Sơn làm. Thêm build/, *.db, *.db-wal, *.db-shm, runtime/ vào gitignore, không ignore fixtures.
- [x] **4. Chạy xanh** `make test NAME=protocol`: exit 0, không warning. So khớp fixture bằng roundtrip; ghi docs wire/opcode/schema/error/ownership như spec, không phát sinh WebSocket.

### Task 2: TCP listener và client C headless — Sơn

**Files:** Create `shared/net.c`, `shared/net.h`, `server/main.c`, `server/app.c`, `server/app.h`, `desktop/tcp_client.c`, `desktop/tcp_client.h`, `tests/test_net.c`, `tests/integration/__init__.py`, `tests/integration/helpers.py`, `tests/integration/test_transport.py`; Modify `Makefile`.

**Interfaces:** Consumes framing Task 1. Produces `Status conn_read(Conn *c, FrameCallback cb, void *ctx)`, `Status conn_queue(Conn *c, const Envelope *e)`, `Status conn_flush(Conn *c)`; Conn sở hữu fd và input/output buffers, hàm `void conn_close(Conn *c)` idempotent. `Status client_connect(Client **out, const char *host, uint16_t port)`, `Status client_send(Client *, const Envelope *)`, `void client_close(Client *)`; Client bọc Conn, connect async, fd/status getter phục vụ GLib. `int app_run(const char *bind_ip,uint16_t port,const char *db_path)`; lúc này chỉ HELLO/PING. Helpers Python `ServerProcess` start binary với `--port 0` đọc actual port từ stdout, nhận thêm danh sách cờ CLI tùy test; `WireClient.request(type,payload)`/`recv_event(type,timeout)` dùng exact frame spec.

- [ ] **1. Viết test** socketpair với send buffer nhỏ để ép EAGAIN: `assert(flushed_bytes_equal_encoded_frames)`; `test_transport` mở 8 client, partial frame/close giữa frame, gửi 2 HELLO trong một write và kiểm tra đúng id; client thứ 65 bị từ chối mà client cũ còn PING được. Output >262144 byte đóng riêng peer chậm.
- [ ] **2. Chạy đỏ** `make test NAME=net` và `make integration NAME=transport`, phải fail ở hành vi TCP chưa có, không chỉ timeout vô hạn; mọi test có timeout 3s.
- [ ] **3. Cài đặt** non-blocking sockets + poll, accept đến EAGAIN, retry EINTR, giữ offset partial write, bỏ SIGPIPE; connect kiểm tra SO_ERROR. Thêm CLI server --bind/--port/--db; cleanup fd/memory khi EOF/error/Ctrl+C. App tạo ConnId generation chống fd reuse. Heartbeat và auth chưa giả lập tại đây.
- [ ] **4. Chạy xanh** cả hai lệnh trên; `make sanitize NAME=net` pass ASan/UBSan. Native Client test dùng server thật và phải trả lỗi connect thay vì treo khi host không tồn tại.

### Task 3: SQLite và transaction trận — Sơn

**Files:** Create `server/store.c`, `server/store.h`, `server/schema.sql`, `tests/test_store.c`; Modify `server/app.c`, `Makefile`.

**Interfaces:** `Status store_open(Store **out,const char *path)`, `void store_close(Store *)`; `Status store_match_start(Store *,const MatchId *,const PlayerSpec players[4],Mode,bool ranked)`; `Status store_events(Store *,const MatchId *,const EventBatch *)`; `Status store_finalize(Store *,const MatchResult *,const ProfileDelta deltas[4])`; `Status store_export_match(Store *,const MatchId *,const char *path)`. Kết quả/events idempotent theo unique matchId và (matchId,seq); duplicate giống dữ liệu OK, khác dữ liệu INVALID_ACTION. Schema version=1, migration additive, không DROP tài khoản để reset test.

- [ ] **1. Viết test** DB temp: `assert(store_finalize_twice_changes_elo_once()); assert(event_seq_unique()); assert(failed_transaction_keeps_old_balances()); assert(reopen_marks_active_aborted());` static helpers kiểm tra SQL trực tiếp từ test connection. Export đủ ordered history, không chứa password/token. Inject lỗi bằng trigger RAISE(ABORT) trong DB test, không thêm config lỗi production.
- [ ] **2. Chạy đỏ** `make test NAME=store`: runner fail do repository chưa thực hiện transaction.
- [ ] **3. Cài đặt** schema §6, prepared statements, foreign_keys ON, busy_timeout=100ms, startup recovery active→aborted; transaction toàn bộ finalize và log. STORAGE_ERROR không publish kết quả thành công. API profile/account/friend tiếp tục bổ sung ở task sở hữu nhưng SQL luôn ở store.c.
- [ ] **4. Chạy xanh** `make test NAME=store` và `make sanitize NAME=store`, đóng/mở DB giữa test vẫn giữ account/log.

### Task 4: Account, session và auth worker — Sơn

**Files:** Create `server/accounts.c`, `server/accounts.h`, `server/auth_worker.c`, `server/auth_worker.h`, `tests/test_accounts.c`, `tests/integration/test_auth.py`; Modify `server/store.c`, `server/store.h`, `server/app.c`, `Makefile`.

**Interfaces:** `Status accounts_handle(App *,ConnId,const Envelope *)`; `Status sessions_user(App *,ConnId,UserId *out)`; `Status sessions_resume(App *,ConnId,const char *token)`; `void sessions_disconnect(App *,ConnId)`; `Status auth_submit(AuthWorker *,ConnId,const AuthJob *)`, `void auth_drain(App *)`. AuthJob sao chép input, AuthResult giữ ConnId và kết quả hash/verify, không giữ pointer tới session. Store thêm `Status store_account_create(Store *,const AccountCreate *,UserId *)`, `Status store_account_get(Store *,const char *email,Account *)`; AccountCreate chứa email/hash/name/avatar, Account chứa ID/hash/profile/ELO; không serialize Account nguyên bản.

- [ ] **1. Viết test** register/login đúng và sai, email trùng khác hoa thường, token giả/expired/logout, tên tiếng Việt 16 và 17 code point, avatar không có, query SQL injection; DB hash khác password. Integration chặn 9 auth job và PING kết nối khác vẫn trả trong 500ms; hoàn thành hash sau disconnect/fd reuse không cấp session cho user mới; repeated id không đổi dữ liệu.
- [ ] **2. Chạy đỏ** `make test NAME=accounts` và `make integration NAME=auth`.
- [ ] **3. Cài đặt** validation/rate limit/password/token ở spec §3; giới hạn auth mặc định 5/phút/IP, chỉnh bằng cờ CLI server `--auth-limit N` vì mọi integration test chạy từ `127.0.0.1` (test nhiều account truyền giá trị lớn, riêng test rate limit dùng mặc định và kiểm tra request thứ 6 trả RATE_LIMITED). Worker một thread queue 8, wake bằng pipe; clear password bằng sodium_memzero sau dùng. Không log request payload auth. LOGIN mới revoke token cũ và disconnect connection cũ, chuyển seat binding an toàn. REGISTER tạo account rồi yêu cầu LOGIN, không tự cấp token.
- [ ] **4. Chạy xanh** hai lệnh trên; `make integration NAME=transport` không regression; kiểm tra shutdown join worker/đóng pipe và không callback tới connection chết.

### Task 5: Phòng và party/invite có quyền host — Sơn

**Files:** Create `server/rooms.c`, `server/rooms.h`, `tests/test_rooms.c`, `tests/integration/test_rooms.py`; Modify `server/app.c`, `Makefile`.

**Interfaces:** `Status rooms_handle(App *,ConnId,const Envelope *)`; `Status rooms_start(App *,const RoomId *)`; `void rooms_disconnect(App *,UserId,uint64_t now_ms)`; `RoomId` string opaque. Produces `RoomStart`={roomId,mode,ranked,players[4]}, `Status matches_start(App *,const RoomStart *,MatchId *out)` declared for Task 9; test substitutes callback in Rooms constructor, not a fake production engine. `Status rooms_make_match(App *,Mode,const UserId users[4],MatchId *)` for queue Task 10. Party={mode,members[2]}, belongs to account state.

- [ ] **1. Viết test** code length=6, collision retry, 5th join ROOM_FULL; nonhost kick/start/bot/mode FORBIDDEN; bot level đúng 3 lựa chọn; host leave chuyển ghế nhỏ nhất; invite accept sau 30s/room đầy/user busy bị từ chối; không cùng lúc hai phòng; party chỉ 2 và không ở solo mode.
- [ ] **2. Chạy đỏ** `make test NAME=rooms`, `make integration NAME=rooms`.
- [ ] **3. Cài đặt** 16 phòng, ghế/membership host, invite room/party accept/reject/expiry; ROOM_START không tự lấp ghế và chỉ giao RoomStart hợp lệ vào callback. Khi callback match start lỗi, giữ room cũ, không set in_match. ROOM_LEAVE trước trận khác MATCH_LEAVE.
- [ ] **4. Chạy xanh** cả hai test; snapshot phòng không có token/email/password, hai người tranh ghế cuối chỉ một người thành công.

### Task 6: Bộ bài và lượt Classic thuần C — Bách

**Files:** Create `server/game.c`, `server/game.h`, `tests/test_game.c`; Reference `client/src/game/{deck.js,rules.js,engine.js,match.test.js}`. Không sửa Makefile (wildcard Task 1). `game.h` định nghĩa `Card`, `Intent`, `Game`.

**Interfaces:** `Status game_init(Game *g,Mode mode,const PlayerSpec players[4],uint64_t seed)`; `Status game_apply(Game *g,Seat actor,const Intent *intent,EventBatch *out)`; `Status game_timeout(Game *g,uint32_t expected_turn,EventBatch *out)`; `Status game_uno_penalty(Game *g,Seat actor,uint32_t watch_generation,EventBatch *out)`. Game lưu UNO watch per seat; hàm trả lỗi không mutate. Timing wall-clock nằm ngoài engine; seed chỉ dùng PRNG shuffle/test, không token.

- [x] **1. Viết test** `assert(deck_count == 112); assert(unique_ids == 112); assert(hand_count(g,0) == 7);` cùng seed cùng deal; conservation 112 sau 1000 action; fixture Skip/Reverse/+2/+4/Discard All/draw-only-new-card/pass/reshuffle/deck cạn. PLAY sai actor/card/màu/turnNo trả lỗi và `memcmp` state trước/sau bằng nhau. Kiểm tra thắng lá cuối như spec.
- [x] **2. Chạy đỏ** `make test NAME=game`.
- [x] **3. Cài đặt** engine không I/O/socket/malloc trong apply, capacity 112 cho hand/deck/discard, validate index trước access; Wild color gửi cùng PLAY. EventBatch ghi nội bộ action, lá được rút, hiệu ứng, turn/winner/stats; không serialize event này trực tiếp cho client.
- [x] **4. Chạy xanh** `make test NAME=game`, `make sanitize NAME=game`; chạy `cd client && npm test` làm baseline tham chiếu, ghi rõ lỗi sẵn có nếu có, không sửa luật JS ngoài phạm vi online.

### Task 7: Team và Draft — Bách

**Files:** Modify `server/game.c`, `server/game.h`; Create `tests/test_modes.c`; Reference `client/src/game/{draft.js,draft.test.js,draft-engine.test.js}`. Không sửa Makefile.

**Interfaces:** Consumes game_apply; adds DRAFT_PICK branch, `Status game_draft_timeout(Game *,uint32_t expected_pick,EventBatch *)`. Game.phase=DRAFT/PLAYING/FINISHED, draft.turn 0..15, offers[4] có occupied flag; teams cố định theo absolute seat.

- [x] **1. Viết test** `assert(team_2v2(0)==team_2v2(2)); assert(team_side(0)==team_side(1));` một người hết bài cả đội thắng; draft init 4 lá, slot bị pick không chọn lại, cả 16 pick đúng công thức spec, sau draft mọi người 8 lá. Wrong actor/old draftTurn không mutate; timeout lấy slot thấp nhất còn bài; PLAY trong DRAFT bị từ chối.
- [x] **2. Chạy đỏ** `make test NAME=modes`.
- [x] **3. Cài đặt** hai cấu hình team và draft trong engine chung, không copy engine thành bốn file/mode; giữ conservation 112 bao gồm offers. Không đổi mode sau match start.
- [x] **4. Chạy xanh** `make test NAME=modes`, `make test NAME=game`, `make sanitize NAME=modes`.

### Task 8: View riêng từng ghế và ba mức bot — Bách

**Files:** Create `server/view.c`, `server/view.h`, `server/bot.c`, `server/bot.h`, `tests/test_view.c`, `tests/test_bot.c`, `tests/fixtures/views.json`. Không sửa Makefile. `view.h` định nghĩa `GameView`.

**Interfaces:** `Status view_build(const Game *,Seat viewer,GameView *out)`; `Status view_json(const GameView *,json_object **out)` (caller sở hữu một ref); `Status bot_choose(const GameView *,BotLevel,uint64_t *rng_state,Intent *out)`. Bot không nhận Game/private deck. GameView chứa public counts và legal actions; bot cùng đường apply như human.

- [x] **1. Viết test** cả 4 viewer x 4 mode: `assert(hidden_hand_absent); assert(hidden_card_ids_absent); assert(no_seed_or_deck);` đúng teammate thấy bài. Bot 3 mức x 100 seed chỉ sinh nước hợp lệ, không có nước thì DRAW/PASS, draft pick đúng slot; hard fixture tránh phạt teammate nếu có nước tốt hơn và chặn enemy gần hết bài.
- [x] **2. Chạy đỏ** `make test NAME=view`, `make test NAME=bot`.
- [x] **3. Cài đặt** whitelist field khi serialize, không copy raw Game rồi xóa vài key. Fixture views phải có viewerSeat=2 để UI không mặc định mình ở ghế 0. Bot scoring chỉ xem dữ liệu visible, tie break deterministic theo RNG test.
- [x] **4. Chạy xanh** hai test và `make sanitize NAME=view`; parse JSON để kiểm tra keys đệ quy, không chỉ tìm chuỗi card có thể trùng ở top public.

### Task 9: Match coordinator, timer và reconnect — Bách; Sơn nối dispatch

**Files:** Create `server/match.c`, `server/match.h`, `tests/test_match.c`, `tests/integration/test_match.py`; Modify `server/{app.c,rooms.c,accounts.c}`, `Makefile`.

**Interfaces:** Produces matches_start Task 5; `Status matches_handle(App *,ConnId,const Envelope *,uint64_t now_ms)`; `void matches_tick(App *,uint64_t now_ms)`; `void matches_disconnect(App *,UserId,uint64_t now_ms)`; `Status matches_resume(App *,ConnId,UserId,uint64_t now_ms)`; `Status matches_finish(App *,const MatchId *,const MatchResult *)` bản đầu chỉ unranked, gọi store_finalize với zero ProfileDelta và ELO giữ nguyên; Task 10 mở rộng ranked, Task 11 thêm progress. Test clock injected qua now_ms, production CLOCK_MONOTONIC. Coordinator sở hữu Game/deadlines per-seat UNO/generation/bot/reconnect; wire snapshot có serverNowMs và deadline cùng miền thời gian.

> **Luật rớt mạng (Bách chốt 11/11):** rớt mạng không có bot đánh thay, không có hạn 60s. Timer lượt 20s chạy tiếp, hết giờ xử lý như timeout thường; vào lại lúc nào cũng lấy lại ghế khi trận còn chạy. Chỉ MATCH_LEAVE mới giao ghế cho bot.
>
> **Chờ Bách xác nhận trước khi viết test task này:** ân hạn UNO 3s là đề xuất của spec §4, ngược quyết định 25/10 (phạt UNO ngay). Nếu giữ quyết định cũ thì bỏ các mốc 2999/3000ms bên dưới, sửa spec cùng lúc.

- [ ] **1. Viết test** thời gian 19999ms chưa phạt, 20000ms phạt; UNO 2999ms chưa phạt, 3000ms +2 đúng một lần; player khác đi không xóa watch; DRAW không reset deadline; callback old turn/match/watch không mutate. Disconnect: ghế không đổi sang bot, không sinh action nào trước 20000ms, tại 20000ms timeout y như người online; rớt mạng ở lượt người khác không ảnh hưởng timer của lượt đó. Resume sau 1s và sau 10 phút (fake clock) đều lấy lại đúng ghế + snapshot hiện tại; explicit leave không reclaim, ghế thành bot; không còn human kết nối→aborted đúng chính sách spec.
- [ ] **2. Chạy đỏ** `make test NAME=match`, `make integration NAME=match` (4 account thật, phòng Classic thật, play/draw tới kết thúc có timeout 60s; ép state chỉ ở unit fixture, không thêm cheat opcode).
- [ ] **3. Cài đặt** identity từ session không từ payload, bind actor đúng seat, stale turn/id reject; tick deadline trước input; clone Game→apply→persist EventBatch→publish. Storage lỗi pause match, không rút bài thêm ở mỗi tick; kết quả chờ finalize callback thành công. Heartbeat 5/15s, ghế người rớt mạng giữ đến hết trận (không timer offline), bot delay700ms chỉ cho ghế bot thật; timer scan tối đa16 match đủ dùng. RESUME gửi snapshot cá nhân, không history bài kín.
- [ ] **4. Chạy xanh** hai test và `make integration NAME=rooms`; integration thêm gửi TIMEOUT/UNO_PENALTY giả → BAD_REQUEST, spoof actor bị từ chối, snapshot chính xác cho từng connection. Kiểm tra SIGTERM đóng sạch, startup sau crash active→aborted.

### Task 10: Matchmaking, ELO và finalize — Sơn

**Files:** Create `server/queue.c`, `server/queue.h`, `tests/test_queue.c`, `tests/test_rating.c`, `tests/integration/test_ranking.py`; Modify `server/{app.c,store.c,store.h,match.c}`, `Makefile`.

**Interfaces:** `Status queue_handle(App *,ConnId,const Envelope *)`, `void queue_remove(App *,UserId)`; consumes rooms_make_match; `int rating_delta(double own,double opposition,bool won)` và `Rank rating_rank(int elo)` trong store.h/store.c (4 enum rank). Mở rộng matches_finish của Task 9: tính MatchResult.eloAfter rồi store_finalize; vẫn zero ProfileDelta cho đến Task 11, không gọi API chưa tồn tại. Task 10 là mốc kiểm chứng matchmaking/rating, chưa được tuyên bố hoàn thành hồ sơ/thành tựu.

- [ ] **1. Viết test** same mode đủ4 mới ghép, different mode không ghép, no bots ranked, FIFO không phá party team, cancel/disconnect xóa whole entry; race join phòng khi queued BUSY. `assert(rating_delta(1500,1500,true)==16); assert(rating_delta(1500,1500,false)==-16);` boundary hạng1199/1200/1399/1400/1799/1800; negative delta làm ELO floor0. Finalize2 lần và DB failure không đổi rating2 lần.
- [ ] **2. Chạy đỏ** `make test NAME=queue`, `make test NAME=rating`, `make integration NAME=ranking`.
- [ ] **3. Cài đặt** FIFO per mode, party placement absolute theo team, validation membership chung rooms/session. Tính theo ELO snapshot đầu trận, lround half away from zero; solo average opponents không giả rank2–4, team average, custom/aborted unchanged. Top20 stable tie userId; finalized transaction duy nhất, MATCH_RESULT sau commit. Lỗi finalize giữ pending, retry theo matchId trước phát kết quả.
- [ ] **4. Chạy xanh** ba test; integration restart server rồi query leaderboard vẫn cùng kết quả, match export đủ lịch sử, không update ELO từ request client.

### Task 11: Bạn bè, presence, hồ sơ và thành tựu — Sơn phần social/store; Bách phần profile

**Files:** Create `server/social.c`, `server/social.h`, `server/profile.c`, `server/profile.h`, `tests/test_social.c`, `tests/test_profile.c`, `tests/integration/test_social.py`; Modify `server/{app.c,store.c,store.h,match.c}`, `Makefile`.

**Interfaces:** `Status social_handle(App *,ConnId,const Envelope *)`; `void social_presence(App *,UserId)`; `Status profile_handle(App *,ConnId,const Envelope *)`; implements `Status profile_compute(Store *,const MatchResult *,ProfileDelta out[4])` (pure read/compute; write chỉ store_finalize). Store thêm `Status store_profile_get(Store *,UserId,Profile *)`, `Status store_profile_update(Store *,UserId,const char *name,const char *avatar)` và API friend operations nội bộ social; Profile chứa public account fields/stats/modes/badges/history, tuyệt đối không có hash/email.

- [ ] **1. Viết test** request/accept/reject friend, self/repeat/reverse request bị từ chối, offline online idle in_match cập nhật; user không sửa hồ sơ người khác. Hai finalize giống matchId chỉ cộng stats một lần; streak thắng/thua và peakElo; các ngưỡng badge 1/100/10/50/300/50/500/1800 chính xác. PROFILE_GET user khác không lộ email/hash/token, pending request chỉ gửi đúng người.
- [ ] **2. Chạy đỏ** `make test NAME=social`, `make test NAME=profile`, `make integration NAME=social`.
- [ ] **3. Cài đặt** friends/presence theo spec; profile counters từ server log/result finalized, không nhận progress client. PROFILE_UPDATE qua store chung, broadcast public name/avatar cập nhật ở room/lobby; avatar ID whitelist và UTF-8 validation dùng chung accounts. Nối profile_compute vào matches_finish trước store_finalize để bỏ zero ProfileDelta của mốc Task 10; rating và progress cùng transaction, không backfill từ dữ liệu demo cũ.
- [ ] **4. Chạy xanh** ba test và `make integration NAME=ranking`; đăng nhập lại sau restart vẫn có friend/name/avatar/badges; STORAGE_ERROR không làm UI nhận profile thành công giả.

### Task 12: Desktop Ubuntu và bridge C thực — Sơn

**Files:** Create `desktop/main.c`, `desktop/bridge.c`, `desktop/bridge.h`, `tests/test_bridge.c`, `docs/ubuntu-demo.md`; Modify `desktop/tcp_client.c`, `Makefile`, `client/vite.config.js` (Bách phối hợp thay base).

**Interfaces:** `Status bridge_init(WebKitWebView *,Client *)`; `Status bridge_dispatch(Client *,const char *json,size_t len)` validates command/envelope; `void bridge_emit(WebKitWebView *,const Envelope *)`; native host registers uno://app scheme. CLI `build/uno-desktop --host 192.168.1.10 --port 9090 --assets ./client/dist`; config không do JS chỉnh. GLib sources xử lý connect/read/write/reconnect + PING, emit CONNECTION_STATE (local event không phải server opcode).

- [ ] **1. Viết test** bridge rejects unknown type/non-main-origin; response chứa quote/newline/`</script>` chỉ trở thành data, không execute code; scheme từ chối `../`, encoded traversal/symlink thoát assets. Native Client test server đóng socket không crash/freeze; GLib callback cleanup khi window đóng. Các test Gtk dùng xvfb, không giả native JS API để kết luận C hoạt động.
- [ ] **2. Chạy đỏ** `make test NAME=bridge` và `xvfb-run -a make desktop-smoke` (smoke harness tự start server, mở webview fixture HELLO rồi exit0 chỉ khi OK nhận về).
- [ ] **3. Cài đặt** GTK3/WebKitGTK4.1 API đã tra ở spec §7; scheme load local asset MIME/path đúng, whitelist bridge, cấm popup/navigation ngoài; serialize event bằng json-c cả phần detail và chuỗi JS. Session token giữ native, RESUME tự động sau reconnect; JS nhận me/state, không nhận token. Backoff1/2/4/8s capped8, thử lại đến khi nối được hoặc người dùng đóng app (không còn hạn 60s); không replay action cũ. Không dùng fetch/WebSocket/Node TCP để thay C.
- [ ] **4. Chạy xanh** hai lệnh trên; chạy app trên Ubuntu desktop thật, đổi host CLI sang máy server LAN và thấy HELLO. Kiểm tra offline host/đóng cửa sổ không treo, JS không gọi file/process arbitrary được.

### Task 13: UI trận nhận state server — Bách

**Files:** Create `client/src/net/bridge.js`, `client/src/net/session.jsx`, `client/src/net/useRemoteGame.js`, `client/src/net/view.js`, `client/src/net/view.test.js`, `client/src/net/bridge.test.js`; Modify `client/src/{main.jsx,pages/GameTable.jsx,pages/MatchResult.jsx}`, `client/src/components/game/{Fan.jsx,MatchClock.jsx,DraftSelection.jsx}`, `client/vite.config.js`; Reference `client/src/game/useUnoGame.js`.

**Interfaces:** `request(type,payload):Promise<envelope.payload>`; `subscribe(type,listener):unsubscribe`. JS cấp bridge-local id, native ánh xạ sang wire id tăng duy nhất cho cả command và PING, response map về local id; counter wire reset chỉ khi mở connection mới. Native giữ token và tự RESUME; JS không thấy token. `toTableView(snapshot):TableView` xoay tuyệt đối→relative; `useRemoteGame():{state,me,isMyTurn,playableIds,canSayUno,turnKey,actions,connectionStatus}` tương thích UI cần dùng, không chạy reducer local. SessionProvider chỉ một listener trong StrictMode; API request timeout10s reject, không auto resend. view adapter tạo discard=[topCard] cho CenterTable, dùng drawCount thay drawPile.length, không tạo deck giả; elapsed display dùng durationMs do coordinator cung cấp cùng serverNowMs.

- [ ] **1. Viết test** fixture viewerSeat=2 cho đủ4 mode: `expect(view.me.seat).toBe(2)` và team giữ nguyên; hidden cardCount7 tạo7 placeholder không có color/value, chuyển action không đổi cardId/turnNo. Duplicate/old revision bỏ, reject request hiện lỗi, unmount cleanup không duplicate listener. Server result là nguồn ELO duy nhất, không gọi applyElo client hoặc buildMatchSummary online. Draft: thời gian qua 20s/settings modal mở không tự gửi pick; chỉ đúng viewer đang lượt được gửi; nhận snapshot pick mới vẫn cập nhật dù animation đang chạy.
- [ ] **2. Chạy đỏ** `cd client && npm test -- src/net/view.test.js src/net/bridge.test.js`.
- [ ] **3. Cài đặt** HashRouter/base tương đối, SessionProvider + adapter, GameTable đổi hook; chọn wild color local rồi gửi một PLAY hợp lệ; pending thao tác chặn double click, chỉ cập nhật authoritative khi snapshot đến. Fan nhận cardCount/visible hand; counts hiệu ứng dùng cardCount thay hand.length; timer hiển thị từ server offset, hết giờ không dispatch action. DraftSelection bỏ timer tự pick/bot/forced pick local, gửi pick ngay khi click trước animation, deadline20s từ server; modal settings không pause trận. DraftTray chỉ hiện mặt bài khi được phép, không dùng draftSeat công thức local trên turn đã xoay. MatchResult dùng MATCH_RESULT; bỏ tính points/ELO từ bài ẩn. View vẫn đủ display properties cho CenterTable/DraftSelection/PlayerBadge/MyHand mà không giả danh tính ghế.
- [ ] **4. Chạy xanh** lệnh test trên, `npm test`, `npm run build`; mở 4 instance desktop cùng phòng để kiểm tra vòng xoay/đội/bài úp, Wild, draft và kết quả thực. Browser dev không có bridge phải hiện thông báo cần desktop, không tự fallback thành ván bot giả.

### Task 14: UI account, phòng, sảnh và hồ sơ dùng API thật — Bách

**Files:** Modify `client/src/pages/{Login.jsx,Lobby.jsx,CustomRoom.jsx,TeamRoom.jsx,Matchmaking.jsx}`, `client/src/context/SocialContext.jsx`, `client/src/components/lobby/{ProfileModal.jsx,FriendsModal.jsx,LeaderboardModal.jsx}`, `client/src/data/{ranks.js,mockData.js}`, `client/src/net/session.jsx`; Create `client/src/net/session.test.js`.

**Interfaces:** Reuse request/subscribe Task13; SocialContext giữ các field UI cần nhưng nguồn từ server, updateProfile/sendFriendRequest trả Promise; bỏ applyElo. PROFILE_STATE/FRIEND_STATE/ROOM_STATE/PARTY_STATE/QUEUE_STATE/LOBBY_STATE làm nguồn dữ liệu. Session không lấy ME/FRIENDS/LEADERBOARD/PROFILE/MATCH_HISTORY mock trong online flow.

- [ ] **1. Viết test** reducer event trong session.test.js: failed login không có me; disconnect không coi user online; room code/bot/host/queue lấy từ event; profile chỉ cập nhật sau OK; `expect(rankOf(1799).key).toBe('gold'); expect(rankOf(1800).key).toBe('diamond')`; `expect(rankKeys).toEqual(['bronze','silver','gold','diamond'])`; empty leaderboard/history render dữ liệu rỗng thay mock.
- [ ] **2. Chạy đỏ** `cd client && npm test -- src/net/session.test.js`.
- [ ] **3. Cài đặt** form REGISTER/LOGIN và host connection status; loại fake timeout matchmaking/invite, fixed room code, localStorage profile và tự tính ELO. Gắn nút join/leave/invite/accept/reject/bot/mode/start vào request, quyền host theo snapshot; lỗi ROOM_FULL/BUSY/NOT_FOUND hiện rõ và giữ màn hình. Profile dùng data thật, tag bất biến, badges theo spec, 4 rank, bỏ level/xp/title giả. Avatar/âm thanh/UI có sẵn tiếp tục dùng; không redesign cả giao diện.
- [ ] **4. Chạy xanh** `npm test`, `npm run build`; manual hai account: đăng ký→kết bạn→mời→accept/reject→room→queue→match→history→đổi avatar→logout/login; không có nút social đang chạy mock còn sót. Cập nhật các caller applyElo/updateProfile bị ảnh hưởng, không chỉ sửa Context.

### Task 15: Kiểm chứng đầy đủ và hướng dẫn demo — cả hai

**Files:** Create `tests/integration/test_full_flow.py`, `tests/integration/test_resilience.py`; Modify `docs/ubuntu-demo.md`, `README.md`, `ARCHITECTURE.md`, `Makefile`. ARCHITECTURE.md hiện untracked: sửa rõ thành tài liệu Ubuntu TCP mới, không vô tình commit cả sơ đồ/tài liệu khác.

**Interfaces:** Lệnh chốt `make all`, `make test`, `make integration`, `make sanitize`, `make desktop-smoke`, `make package`; package là tar.gz chứa binary native + React dist + launcher + hướng dẫn cài runtime Ubuntu, không hứa binary static/AppImage. Launcher định vị assets theo thư mục mình, host/port qua CLI.

- [ ] **1. Viết test** full_flow dùng WireClient từ Task2, tài khoản/DB temp thật: register4→friend→party team→queue→play full match→result→leaderboard/history/profile→restart→query; test_resilience kiểm tra partial frames/slow reader/client kill/15s heartbeat/resume giữa trận lấy lại ghế/SQL failure/finalize duplicate và 16 phòng không ngắt phòng khác. Test deadline unit dùng fake clock; chỉ integration heartbeat dùng thời gian thực có timeout hữu hạn.
- [ ] **2. Chạy đỏ** `make integration NAME=full_flow`, `make integration NAME=resilience`; ghi chức năng còn thiếu cụ thể, không skip để lấy xanh.
- [ ] **3. Hoàn thiện tích hợp tối thiểu** sửa caller/exports/fixture/build bị lệch contract; docs cài `build-essential pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev libjson-c-dev libsqlite3-dev libsodium-dev python3 xvfb`, Node22/npm theo môi trường; dùng `npm ci` và lockfile hiện có. Server bind0.0.0.0, ghi IP LAN/cổng9090, cách mở firewall khi cần, DB path, dừng/export log. Không tự đổi rule hoặc cài thêm framework. ARCHITECTURE.md bỏ WS/HTTP static/CLI transport kép cũ.
- [ ] **4. Chạy xanh** trên Ubuntu: `make all`, `make test`, `make integration`, `make sanitize`, `xvfb-run -a make desktop-smoke`, `cd client && npm test && npm run build`, `make package`. Mỗi lệnh exit0; ASan/UBSan không lỗi. README ghi đúng những kiểm chứng đã thực sự chạy.
- [ ] **5. Demo LAN thật** máyA server+desktop, máyB desktop; thêm2 human instance hoặc bot trong custom, không gọi ranked nếu có bot. Chơi đủ4 mode, 3 bot level, timeout/UNO, rút mạng/rejoin, host leave/kick/invite, queue4 human, persistence profile/ELO/history sau restart. Máy chạy server phải là Ubuntu thật hoặc WSL2 bật mirrored networking/port forward 9090; WSL2 NAT mặc định máy khác không kết nối vào được. Lưu bảng pass/fail, IP/Ubuntu version và giới hạn chưa kiểm chứng; không coi loopback/headless là LAN thật.

## Hoàn thành khi

Mỗi hàng trong bảng chức năng người dùng có task tương ứng: bài/luật6–7; timer/UNO9; authoritative TCP1–2/8–9; modes7; bot8; disconnect9; UI12–14; log3/9–10; profile/thành tựu11/14; listener2; matchmaking10; ELO10/14; friends11/14; account4/14; presence11/14; room/invite5/14. Không có task nào tùy chọn nếu muốn tuyên bố hoàn thành toàn bộ project.

## Tự rà soát bản plan

- Đã đối chiếu toàn bộ bảng Bách–Sơn; bảo toàn UI làm một người, schema/network làm một người.
- Đã đặt cùng tên API giữa producer/consumer; type đi qua ranh giới Bách–Sơn định nghĩa ở Task1, model nội bộ engine ở Task 6/8, opaque state thuộc module sở hữu.
- Đã thêm test cho 5 Review Focus, đặc biệt UNO nhiều ghế, retry finalize và auth fd reuse.
- Đã thay bước commit của mẫu bằng kiểm chứng theo yêu cầu người dùng. Không thực hiện code/commit/branch trong lượt lập plan.
- Chưa thực thi Ubuntu build/GUI/LAN. WebKitGTK bridge và local asset load là mốc kiểm chứng sớm; nếu không đạt, báo bằng evidence trước khi đổi nền tảng.

## Cách thực thi để người dùng chọn sau khi duyệt

- **Native (đề xuất):** triển khai trực tiếp từng task trong chat này, review các mốc tích hợp; phù hợp vì nhiều task phụ thuộc cùng contract C và một schema. Review độc lập cuối khi người dùng cho phép cách thực thi này; không mặc định đổi model.
- **Subagent-driven:** mỗi task một implementer và reviewer độc lập, kiểm soát ownership file chung; nhiều lượt review/context hơn. Chỉ dùng khi người dùng chọn, chưa dispatch agent ở lượt lập plan.

Chờ người dùng duyệt thiết kế/plan và chọn cách thực thi. Quyền commit/tạo branch vẫn chưa có, kể cả sau khi được phép bắt đầu code nếu không được cho phép riêng.
