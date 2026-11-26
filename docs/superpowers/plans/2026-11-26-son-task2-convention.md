# Sơn — Task 2: chuẩn hóa convention cho TCP, account, lobby, SQLite và desktop native

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Đưa toàn bộ C/header và test C thuộc phần Sơn về [QUY_UOC_CONVENTION.md](../../../QUY_UOC_CONVENTION.md) trên đường dẫn hiện tại, cộng một thay đổi hành vi duy nhất: kiểm tra kết quả `send()` khi từ chối kết nối `SERVER_FULL`.

**Architecture:** Mỗi task xử lý một nhóm module cùng test của nó và tự build được. Chỉ đổi tên **nội bộ file** (hàm `static`, biến cục bộ, tham số, biến file-scope, field của struct định nghĩa trong `.c`). Tên trong header và field của struct public giữ nguyên cho Task 3 của plan tổng, nên mọi caller ở module khác (kể cả phần gameplay của Bách) không cần sửa. Thay đổi `SERVER_FULL` nằm trong commit riêng.

**Tech Stack:** C11, POSIX socket/poll, json-c, SQLite3, libsodium, GTK3/WebKitGTK 4.1 (chỉ `desktop/` và `test_bridge`), Python 3 unittest.

**Spec:** [Kế hoạch refactor tổng](2026-11-25-c-convention-folder-refactor.md) — mục Task 2 và Task 3. Convention: [QUY_UOC_CONVENTION.md](../../../QUY_UOC_CONVENTION.md).

**Branch:** `refactor/son-task2-convention`, tạo từ `main` @ `31f61ab` (đã có Task 1 và phần chuyển file gameplay của Bách).

## Global Constraints

- Commit message **không** có dòng `Co-Authored-By`. Tiền tố theo nhóm: `refactor(c):`, `docs(c):`, `fix(net):`.
- Build `-std=c11 -Wall -Wextra -Wpedantic -Werror`. Server/test headless phải build khi không có GTK.
- Giữ nguyên: giao thức UNOP/1, opcode, JSON key, mã lỗi, nội dung SQL và schema, thứ tự giao dịch SQLite, CLI, tên binary, path asset, sự kiện bridge, trị số mọi hằng số.
- Không đổi tên nào xuất hiện trong `.h` (hàm, kiểu, field, enum). Ví dụ `conn_flush`, `Conn.out_buf`, `App.listen_fd`, `Client.on_pending_cb` giữ nguyên tới Task 3.
- Không sửa file của Bách (`server/game/*`, `tests/unit/*`) và không sửa `Makefile`.
- `desktop/main.c`: chỉ đổi tên, comment và format ở phần kết nối, heartbeat, reconnect. Khối dựng cửa sổ/WebView trong `main()` và thân `on_decide_policy`, `on_load_changed`, `on_window_destroy` giữ nguyên, trừ tham chiếu đến biến đã đổi tên.

## Quy tắc áp dụng cho mọi task

**Format** (khớp phần Bách đã làm ở `ac765c3`):
- 4 spaces, không tab. Dấu `{` cùng dòng với hàm và câu điều khiển: `Status conn_flush(Conn *c) {`.
- Dòng tối đa 100 cột. Không tách hay sửa nội dung string literal (SQL, JSON key, thông báo lỗi). Nếu literal làm dòng dài, xuống dòng trước literal. Literal một mình vượt 100 cột thì để nguyên.
- `if (x) return y;` một dòng được giữ như hiện tại.

**Tên:**
- Hàm/biến/tham số/field nội bộ: `camelCase`. Ví dụ `valid_body_bytes → validBodyBytes`, `uid_str → userIdText`.
- Biến file-scope `g_xxx_yyy → gXxxYyy`. Ví dụ `g_stop_requested → gStopRequested`, `g_assets_dir → gAssetsDir`.
- Hằng số: `UPPER_SNAKE_CASE` qua `enum` (số nguyên) hoặc `static const double` (số thực), đặt đầu file `.c`.
- Bỏ tên một chữ cái mơ hồ (`s`, `c`, `n`, `e`, `st`, `ret`, `tmp`), trừ chỉ số vòng lặp `i/j/k`. Gợi ý: `Status s → status`, `const Envelope *e → envelope`, `Conn *c → connection`.
- Hàm test `test_xxx → testXxx`, helper test cũng camelCase.
- Giữ nguyên tên thư viện/POSIX (`json_object_*`, `sqlite3_*`, `crypto_*`, `g_*`/`gtk_*` của GLib/GTK) và `main`.
- Khi tham số trùng tên field public (ví dụ `slot->conn_id = conn_id`), chỉ đổi tham số: `slot->conn_id = connId`.

**Comment:**
- Comment đầu hàm Doxygen (`@brief`, `@param[in/out]`, `@return`) cho mọi hàm trong header và các hàm `static` có điều kiện không hiển nhiên. Bắt buộc nêu: ai sở hữu/giải phóng JSON và buffer, fd có bị đóng hay không, hàm có chặn không, trả về gì khi EOF/partial I/O.
- Comment trong thân hàm giải thích *vì sao* (byte order, non-blocking, retry `EINTR`, `MSG_NOSIGNAL`, rollback). Xóa comment chỉ lặp lại code.
- Lỗi chưa xử lý nhưng ngoài phạm vi: ghi `/* FIXME: <vấn đề>. <hướng xử lý>. */`, không sửa hành vi.

## Môi trường kiểm chứng

Working tree trên Windows bị `core.autocrlf=true` (Makefile, `app.c` có CRLF), nên `make` không chạy trực tiếp trên `/mnt/f`. Mọi lệnh kiểm chứng chạy trong bản sao LF ở WSL Ubuntu-22.04; nghiệm thu cuối cùng chạy trên Ubuntu 24.04 theo Task 5 plan tổng.

Hàm `unoSync` dưới đây đặt vào `~/.bashrc` trong WSL. Nó đưa bản sao `~/uno-verify` về `HEAD` của nhánh cộng các thay đổi chưa commit:

```bash
unoSync() {
    local repo=/mnt/f/ltm_prj/UNO-multiplayer
    [ -d ~/uno-verify ] || git clone -q "$repo" ~/uno-verify
    cd ~/uno-verify || return 1
    git fetch -q "$repo" refactor/son-task2-convention && git reset -q --hard FETCH_HEAD
    git -C "$repo" diff HEAD --binary | git apply --allow-empty
}
```

Mọi bước "Run" dưới đây có dạng `wsl -d Ubuntu-22.04 -- bash -lic 'unoSync && <lệnh>'`. Plan chỉ ghi phần `<lệnh>`.

## Review Focus

1. **Đổi tên làm lệch nghĩa ở chỗ shadowing.** Đổi `c → connection` trong hàm có biến `connection` khác cùng scope sẽ build được nhưng sai đối tượng. Mỗi đổi tên dựa vào compiler (`-Werror` bắt biến chưa dùng) và test của module. Không dùng `sed` toàn file cho tên ngắn.
2. **SERVER_FULL khi peer đã đóng.** Gửi vào socket mà peer đã RST phải không gây SIGPIPE và không làm vòng `accept` dừng. Test ở Task 2.
3. **Nội dung SQL bị đổi do format.** `store.c` có 82 dòng dài, phần lớn là SQL. Task 6 so sánh các literal trước/sau bằng lệnh `strings`.
4. **Thứ tự comment/format trong vòng poll** không được đổi thứ tự `auth pipe → listen socket → client sockets → sweep`. Task 2 giữ nguyên thứ tự khối.
5. **Bridge vẫn không lộ token sang JS** sau khi đổi tên trong `net_session.c`/`bridge.c`. `make test NAME=bridge` và `NAME=net_session` có ca cho việc này. Task 7 chạy cả hai.

---

### Task 0: Ghi baseline

**Files:** không sửa file nào trong repo.

- [ ] **Step 1: Cài `unoSync` trong WSL và đồng bộ bản sao về `31f61ab`.**

Run: `unoSync && git log -1 --format=%h`
Expected: `31f61ab`

- [ ] **Step 2: Chạy toàn bộ bộ test của Sơn và lưu kết quả.**

Run:
```bash
unoSync && make -B server && for name in protocol net accounts rooms queue rating social store net_session bridge; do make test NAME="$name" > /tmp/base-$name.log 2>&1; echo "$name $?"; done; make integration NAME=transport > /tmp/base-transport.log 2>&1; echo "transport $?"
```
Expected: mỗi dòng in exit code. Nếu WSL thiếu GTK/WebKit, `bridge` không có trong `test_names` và báo lỗi target; ghi lại "bridge: chỉ kiểm trên Ubuntu 24.04 có GTK".

- [ ] **Step 3: Ghi baseline vào mô tả PR nháp** (SHA, exit code từng test, lỗi có sẵn nếu có). Không commit file log.

---

### Task 1: `shared/` — protocol, net, contracts

**Files:**
- Modify: `shared/contracts.h`, `shared/protocol.c`, `shared/protocol.h`, `shared/net.c`, `shared/net.h`
- Test: `tests/test_protocol.c`, `tests/test_net.c` (chỉ các hàm test cho `Conn`, phần client để Task 7)

**Interfaces:**
- Consumes: không.
- Produces: không đổi chữ ký nào. Hằng số mới chỉ nằm trong `net.c`.

- [ ] **Step 1: Đổi tên nội bộ `shared/protocol.c`.** `number_length → numberLength`, `valid_body_bytes → validBodyBytes`, cùng biến cục bộ và tham số theo quy tắc chung.

- [ ] **Step 2: Đổi tên nội bộ `shared/net.c` theo bảng.**

| Cũ | Mới | Ghi chú |
|---|---|---|
| tham số `Conn *c` | `connection` | mọi hàm `conn_*` |
| `uint8_t buf[16384]` | `receiveBuffer[RECEIVE_BUFFER_SIZE]` | `enum { RECEIVE_BUFFER_SIZE = 16384 };` |
| `ssize_t n` (recv) | `bytesReceived` | |
| `Status s` | `status` | |
| `uint8_t tmp[FRAME_CAPACITY]` | `frameBuffer` | |
| `written` | `frameLength` | |
| `to_send` | `bytesToSend` | |
| `ssize_t sent` | `bytesSent` | |
| `FrameCallback cb`, `void *ctx` | `onFrame`, `context` | chỉ trong `.c`; tên tham số trong `.h` sửa cùng để khớp Doxygen |

Tên tham số trong prototype ở `.h` không phải identifier liên kết, đổi được ngay để khớp comment.

- [ ] **Step 3: Comment `shared/net.h` và `shared/protocol.h`.** Mỗi hàm có Doxygen nêu:
  - `conn_read`: đọc tới `EAGAIN`, gọi callback cho từng frame đủ, `recv` trả 0 là EOF → đặt `closing`, trả `BAD_REQUEST`. Callback lỗi → ngừng đọc và trả lỗi đó.
  - `conn_queue`: copy frame vào `out_buf`, nén phần đã gửi trước. Vượt `CONN_OUT_BUF_SIZE` → `closing` và `RATE_LIMITED` (slow consumer).
  - `conn_flush`: gửi tới hết hoặc `EAGAIN`; partial send giữ `out_pos`; trả `OK` khi còn dữ liệu chờ `POLLOUT`.
  - `conn_close`: idempotent, đóng fd đúng một lần.
  - `frame_feed`/`frame_encode`: độ dài big-endian 4 byte, giới hạn `FRAME_CAPACITY`, buffer thuộc caller.
  - Trong `conn_init`: thêm `/* FIXME: fcntl(F_SETFL) failure is ignored, leaving a blocking socket in the poll loop. Return BAD_REQUEST and let the caller close. */`.

- [ ] **Step 4: Format bốn file theo quy tắc chung; `contracts.h` chỉ format/comment.**

- [ ] **Step 5: Đổi tên hàm test trong `tests/test_protocol.c` và các hàm test `Conn` trong `tests/test_net.c`.** Ví dụ `test_partial_write_and_flush → testPartialWriteAndFlush`, `on_frame_received → onFrameReceived`, `set_nonblocking → setNonBlocking`. Assertion giữ nguyên.

- [ ] **Step 6: Chạy test.**

Run: `unoSync && make -B server && make test NAME=protocol && make test NAME=net`
Expected: exit 0, cùng số ca như baseline.

- [ ] **Step 7: Commit.**

```bash
git add shared/ tests/test_protocol.c tests/test_net.c
git commit -m "refactor(c): apply conventions to shared protocol and net"
```

---

### Task 2: Server TCP — `app.*`, `main.c` và kiểm tra `send()` của `SERVER_FULL`

**Files:**
- Modify: `server/app.c`, `server/app.h`, `server/main.c`
- Test: `tests/test_net.c` (thêm 2 ca), `tests/integration/test_transport.py` (chạy, không sửa)

**Interfaces:**
- Consumes: `frame_encode`, `conn_*` từ Task 1 (tên cũ).
- Produces: `Status appRejectConnection(int connectionSocket);` trong `server/app.h`. Trả `OK` khi gửi đủ frame `SERVER_FULL`, `BAD_REQUEST` khi `send()` lỗi hoặc gửi thiếu. Luôn đóng `connectionSocket` trước khi trả về.

- [ ] **Step 1: Đổi tên nội bộ `server/app.c`.**

| Cũ | Mới |
|---|---|
| `g_stop_requested`, `handle_sig` | `gStopRequested`, `handleStopSignal` |
| `FrameContext.slot_idx` (struct trong `.c`) | `slotIndex` |
| `int fd` trong `app_init` | `listenSocket` |
| `opt` | `reuseAddress` |
| `addr` | `serverAddress` |
| `bound_addr`, `addr_len` | `boundAddress`, `addressLength` |
| `int pfd[2]` trong `app_init` | `authPipe[2]` |
| `pfd`, `nfds`, `slot_map` trong `app_run` | `pollFds`, `pollCount`, `slotByPollIndex` |
| `int ret = poll(...)` | `readyCount` |
| `cfd` | `connectionSocket` |
| `client_addr`, `clen` | `clientAddress`, `clientAddressLength` |
| `free_slot` | `freeSlot` |
| `s_idx`, `st` | `slotIndex`, `status` |
| `app_announce_shutdown`, `app_match_ended`, `app_match_push`, `app_on_frame`, `app_on_match_start`, `app_sweep_idle` | `announceShutdown`, `onMatchEnded`, `onMatchPush`, `onFrame`, `onMatchStart`, `sweepIdleConnections` |
| `dispatch_watched`, `in_list`, `is_match_command`, `is_room_command`, `is_social_command`, `live_slot`, `record_in_match`, `record_match`, `send_envelope`, `status_message` | `dispatchWatched`, `inList`, `isMatchCommand`, `isRoomCommand`, `isSocialCommand`, `liveSlot`, `recordInMatch`, `recordMatch`, `sendEnvelope`, `statusMessage` |
| `watch_add`, `watch_begin`, `watch_end`, `watch_inviter` | `watchAdd`, `watchBegin`, `watchEnd`, `watchInviter` |

Hằng số mới trong `app.c`, giữ trị số:

```c
enum {
    LISTEN_BACKLOG = 128,
    POLL_TIMEOUT_MS = 50,
    SERVER_FULL_FRAME_SIZE = 256
};
static const double RATE_TOKENS_PER_MS = 0.03;
static const double RATE_BURST_TOKENS = 60.0;
```

`RATE_BURST_TOKENS` thay cả hai chỗ `60.0` (giới hạn bucket và giá trị khởi tạo slot mới).

- [ ] **Step 2: `server/main.c`.** `bind_ip`, `db_path`, `auth_limit`, `heartbeat_ms` → `bindIp`, `dbPath`, `authLimit`, `heartbeatMs`. Thêm `enum { DEFAULT_SERVER_PORT = 9090, DEFAULT_AUTH_LIMIT = 5 };` và `static const char DEFAULT_BIND_IP[] = "0.0.0.0";`. Chuỗi usage và tên cờ CLI giữ nguyên byte.

- [ ] **Step 3: Comment.**
  - `app_init`: comment theo bước socket → `SO_REUSEADDR` (vì sao: restart không chờ TIME_WAIT) → non-blocking (vì sao: vòng `accept` đọc tới `EAGAIN`) → `htons(port)` network byte order → bind → listen(`LISTEN_BACKLOG`) → `getsockname` + `ntohs` cho cổng ephemeral in ra `PORT n`.
  - Thêm FIXME: kết quả `setsockopt` và `fcntl(F_SETFL)` của listen socket, kết quả `fcntl` trên auth pipe, và việc `pipe()` thất bại thì không có auth worker. Mỗi FIXME nêu hướng xử lý (trả `BUSY` và đóng socket).
  - Vòng `accept`: `EINTR` thử lại, `EAGAIN/EWOULDBLOCK` hết hàng đợi, lỗi khác (`ECONNABORTED`, `EMFILE`) thoát vòng để poll lần sau — ghi đúng hành vi hiện tại.
  - `app.h`: Doxygen cho `app_init`, `app_push`, `app_reply_*`, `app_close_slot`, `app_conn_check_rate_limit` (token bucket 0.03 token/ms, tối đa 60), `app_now_ms` (CLOCK_MONOTONIC), `app_user_leave`.

- [ ] **Step 4: Format `app.c`, `app.h`, `main.c`. Giữ nguyên thứ tự khối trong `app_run`.**

- [ ] **Step 5: Chạy test và commit phần đổi tên/format** (chưa đổi hành vi).

Run: `unoSync && make -B server && make test NAME=net && make test NAME=accounts && make integration NAME=transport`
Expected: exit 0; `test_max_connections` pass.

```bash
git add server/app.c server/app.h server/main.c
git commit -m "refactor(c): apply conventions to server listener and dispatcher"
```

- [ ] **Step 6: Viết hai ca test thất bại trong `tests/test_net.c`.**

```c
#include "server/app.h"

static void testRejectConnectionSendsServerFull(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(appRejectConnection(pair[0]) == OK);
    /* pair[0] must be closed by the call. */
    assert(fcntl(pair[0], F_GETFD) == -1 && errno == EBADF);
    // Đọc hết pair[1], đưa qua frame_feed; callback giữ envelope đầu tiên.
    // assert type == "ERROR", payload.code == "SERVER_FULL",
    //        payload.message == "Server connection limit reached", v == 1, id == 0.
    close(pair[1]);
}

static void testRejectConnectionReportsBrokenPeer(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    close(pair[1]);
    /* MSG_NOSIGNAL: EPIPE comes back as an error instead of killing the test. */
    assert(appRejectConnection(pair[0]) == BAD_REQUEST);
    assert(fcntl(pair[0], F_GETFD) == -1 && errno == EBADF);
}
```

Gọi cả hai trong `main()` của `test_net.c`.

- [ ] **Step 7: Chạy để thấy lỗi.**

Run: `unoSync && make test NAME=net`
Expected: lỗi build `implicit declaration of function 'appRejectConnection'`.

- [ ] **Step 8: Cài đặt `appRejectConnection` trong `server/app.c`, khai báo trong `server/app.h`.**

Chuyển nguyên khối encode + `send` + `close` ở nhánh `freeSlot < 0` vào hàm này; nhánh đó chỉ còn `appRejectConnection(connectionSocket); continue;`. Giữ một lần `send(..., MSG_NOSIGNAL)` duy nhất, không retry (best-effort, socket vừa accept còn blocking và frame nhỏ hơn `SERVER_FULL_FRAME_SIZE`). Chẩn đoán in ra `stderr` (stdout dành cho dòng `PORT n`):
- `bytesSent < 0`: `"SERVER_FULL reject: send failed: %s\n"` với `strerror(errno)`.
- `0 <= bytesSent < frameLength`: `"SERVER_FULL reject: sent %zd of %zu bytes\n"`.
- `frame_encode` thất bại: không gửi, trả `BAD_REQUEST`.

`json_object_put(payload)` và `close(connectionSocket)` chạy trên mọi nhánh. Doxygen trong `app.h` nêu: hàm sở hữu và luôn đóng socket, chặn tối đa một lần `send`, không bao giờ phát SIGPIPE.

- [ ] **Step 9: Chạy test.**

Run: `unoSync && make -B server && make test NAME=net && make integration NAME=transport && make integration NAME=resilience`
Expected: exit 0. `test_max_connections` vẫn nhận `SERVER_FULL`.

- [ ] **Step 10: Commit riêng thay đổi hành vi.**

```bash
git add server/app.c server/app.h tests/test_net.c
git commit -m "fix(net): report failed SERVER_FULL rejection sends"
```

---

### Task 3: Account và xác thực — `accounts.*`, `auth_worker.*`

**Files:**
- Modify: `server/accounts.c`, `server/accounts.h`, `server/auth_worker.c`, `server/auth_worker.h`
- Test: `tests/test_accounts.c`

**Interfaces:** không đổi chữ ký public.

- [ ] **Step 1: Đổi tên nội bộ.** `accounts_send_error → sendAccountError`, `find_live_token → findLiveToken`, `get_now_ms → monotonicNowMs` (bọc `accounts_get_now_ms`, CLOCK_MONOTONIC cộng offset 100 ngày; `rooms.c` có bản sao cùng tên, đổi giống vậy ở Task 4), `base_offset_ms → baseOffsetMs`, `release_other_account → releaseOtherAccount`, `auth_worker_thread → authWorkerThread`. Biến: `uid_str → userIdText`, `tag_str → tagText`, `j_pass/j_email/j_token/j_name/j_avatar → passwordJson/emailJson/tokenJson/nameJson/avatarJson`, `req_id → requestId`, `old_conn → previousConn`, `free_entry → freeEntry`, `seq_len → sequenceLength`, `cp_count → codePointCount`, `byte_len → byteLength`, `dummy_hash/dummy_ret → dummyHash/dummyResult`, `notify_pipe_fd → notifyPipeFd`. Struct `RateLimiter`/`Sessions` nếu định nghĩa trong `accounts.c` thì đổi field của chúng; struct trong `.h` giữ nguyên. Macro `MAX_RATE_LIMIT_IPS`, `MAX_TIMESTAMPS_PER_IP`, `AUTH_QUEUE_CAPACITY` giữ.

- [ ] **Step 2: Comment.** Doxygen cho `accounts_handle`, `sessions_*`, `auth_submit`, `auth_drain`: hash Argon2 chạy trên worker thread, kết quả về poll thread qua pipe non-blocking; token sinh bởi libsodium, chỉ trả cho connection đã xác thực; `sessions_resume` giữ ghế và từ chối token hết hạn. Comment trong thân: vì sao dùng `dummyHash` (cân bằng thời gian khi email không tồn tại).

- [ ] **Step 3: Format, đổi tên hàm test trong `tests/test_accounts.c`.**

- [ ] **Step 4: Chạy test.**

Run: `unoSync && make -B server && make test NAME=accounts && make integration NAME=auth`
Expected: exit 0.

- [ ] **Step 5: Commit.**

```bash
git add server/accounts.* server/auth_worker.* tests/test_accounts.c
git commit -m "refactor(c): apply conventions to accounts and auth worker"
```

---

### Task 4: Lobby — `rooms.*`, `queue.*`

**Files:**
- Modify: `server/rooms.c`, `server/rooms.h`, `server/queue.c`, `server/queue.h`
- Test: `tests/test_rooms.c`, `tests/test_queue.c`

**Interfaces:** không đổi chữ ký public. `RoomStart`, `OnMatchStart` mà `server/game/match.c` dùng giữ nguyên.

- [ ] **Step 1: Đổi tên nội bộ `rooms.c`.** 16 hàm static (`get_now_ms → monotonicNowMs`, `generate_uuid → generateUuid`, `send_error → sendError`, `send_ok → sendOk`, `reject_if_in_match → rejectIfInMatch`, `push_to_seated_humans → pushToSeatedHumans`, …). Biến `*_obj`/`*_arr` (`u_obj`, `s_obj`, `m_obj`, `seats_arr`) đặt theo nội dung: `userJson`, `seatJson`, `modeJson`, `seatsJson`. `resp_payload → responsePayload`, `push_env → pushEnvelope`, `target_uid → targetUserId`, `rc_conn → recipientConn`, `inv_slot → inviteSlot`, `p_slot → partySlot`, `now_ms_val → nowMs`.

- [ ] **Step 2: Đổi tên nội bộ `queue.c`.** 22 hàm static (`try_start → tryStart`, `place_seats → placeSeats`, `first_offline_seat → firstOfflineSeat`, …), `n_entries → entryCount`, `idx_out/seats_out/mode_out → outIndex/outSeats/outMode`.

- [ ] **Step 3: Comment.** `rooms.h`/`queue.h`: Doxygen cho các hàm public, nêu room riêng luôn unranked, queue FIFO theo mode, party 2 người chỉ ở `2v2`/`side`, `queue_tick` nhận `nowMs` monotonic. Trong `rooms.c` comment vì sao mã phòng 6 ký tự và invite UUID.

- [ ] **Step 4: Format; 3 dòng dài ở `rooms.h` và 19 ở `rooms.c` xuống dòng theo quy tắc. Đổi tên hàm test trong hai file test.**

- [ ] **Step 5: Chạy test.**

Run: `unoSync && make -B server && make test NAME=rooms && make test NAME=queue && make integration NAME=rooms`
Expected: exit 0.

- [ ] **Step 6: Commit.**

```bash
git add server/rooms.* server/queue.* tests/test_rooms.c tests/test_queue.c
git commit -m "refactor(c): apply conventions to rooms and queue"
```

---

### Task 5: `rating.*`, `social.*`

**Files:**
- Modify: `server/rating.c`, `server/rating.h`, `server/social.c`, `server/social.h`
- Test: `tests/test_rating.c`, `tests/test_social.c`

**Interfaces:** `rating_apply`, `rating_delta`, `rating_rank*` mà `server/game/match.c` gọi giữ nguyên tên và chữ ký.

- [ ] **Step 1: Đổi tên nội bộ.** `rating_average → averageRating`, `rating_is_team_mode → isTeamMode`, `rating_seat_delta → seatDelta`. `social.c`: 29 hàm static (`handle_profile_get → handleProfileGet`, `friend_row_json → friendRowJson`, `parse_user_id_value → parseUserIdValue`, …).

- [ ] **Step 2: Comment.** `rating.h`: ELO khởi đầu 1500, K=32, `lround`, clamp ≥ 0; solo so với trung bình 3 người còn lại; team dùng trung bình đội. Ngưỡng hạng Đồng < 1200, Bạc 1200..1399, Vàng 1400..1799, Kim cương ≥ 1800. `social.h`: snapshot không chứa email/hash/token; ai sở hữu `json_object` trả về.

- [ ] **Step 3: Format, đổi tên hàm test.**

- [ ] **Step 4: Chạy test.**

Run: `unoSync && make -B server && make test NAME=rating && make test NAME=social && make test NAME=match && make integration NAME=social && make integration NAME=ranking`
Expected: exit 0. `NAME=match` (test của Bách) chứng minh seam rating không đổi.

- [ ] **Step 5: Commit.**

```bash
git add server/rating.* server/social.* tests/test_rating.c tests/test_social.c
git commit -m "refactor(c): apply conventions to rating and social"
```

---

### Task 6: SQLite — `store.*`

**Files:**
- Modify: `server/store.c`, `server/store.h`
- Test: `tests/test_store.c`
- Không sửa: `server/schema.sql`

**Interfaces:** mọi `store_*` public giữ nguyên. Field của `struct Store` (định nghĩa trong `store.c`) được đổi.

- [ ] **Step 1: Chụp string literal trước khi sửa.**

Run: `unoSync && gcc -E -P -I. -Ishared $(pkg-config --cflags json-c sqlite3 libsodium) server/store.c | grep -o '"[^"]*"' | tr -d '\n' | sed 's/""//g' > /tmp/store-literals-before.txt`
Expected: file khác rỗng.

- [ ] **Step 2: Đổi tên nội bộ.** Field `stmt_xxx_yyy → xxxYyyStmt` (ví dụ `stmt_account_insert → accountInsertStmt`, `stmt_profile_stats_select → profileStatsSelectStmt`). Biến SQL `sql_xxx → xxxSql` với tên đầy đủ (`sql_m_ins → matchInsertSql`, `sql_p_sel_seat → playerSelectBySeatSql`, `sql_f_acc → friendAcceptSql`, `sql_prof_fb → profileStatsInsertFallbackSql`). `p_stmt/m_stmt/e_stmt → playerStmt/matchStmt/eventStmt`, `step_rc → stepResult`, `cur_status → currentStatus`. 18 hàm static (`rollback_with → rollbackWith`, `migrate_schema → migrateSchema`, `finalize_statements → finalizeStatements`, …). `EMBEDDED_SCHEMA_SQL`, `STORE_SCHEMA_VERSION`, `PROFILE_WON_SQL` giữ.

- [ ] **Step 3: Comment.** `store.h`: Doxygen nêu hàm nào mở transaction, rollback khi nào, `store_finalize` idempotent theo `match_id` (gọi lặp không cộng ELO hai lần), `store_export_match` trả JSON caller phải `json_object_put`. Trong `store.c`: vì sao migration theo `user_version`, vì sao prepare tất cả statement lúc mở.

- [ ] **Step 4: Format; 82 dòng dài xử lý theo quy tắc literal.**

- [ ] **Step 5: So sánh literal.**

Run: lặp lại lệnh Step 1 ra `/tmp/store-literals-after.txt`, rồi `cmp /tmp/store-literals-before.txt /tmp/store-literals-after.txt`
Expected: `cmp` không in gì, exit 0.

- [ ] **Step 6: Đổi tên hàm test trong `tests/test_store.c` (1619 dòng, 20 dòng dài).**

- [ ] **Step 7: Chạy test.**

Run: `unoSync && make -B server && make test NAME=store && make test NAME=match && make test NAME=profile && make integration NAME=ranking && make integration NAME=match`
Expected: exit 0.

- [ ] **Step 8: Commit.**

```bash
git add server/store.c server/store.h tests/test_store.c
git commit -m "refactor(c): apply conventions to SQLite store"
```

---

### Task 7: Desktop native — `tcp_client.*`, `net_session.*`, `bridge.*`, `main.c`

**Files:**
- Modify: `desktop/tcp_client.c`, `desktop/tcp_client.h`, `desktop/net_session.c`, `desktop/net_session.h`, `desktop/bridge.c`, `desktop/bridge.h`, `desktop/main.c`
- Test: `tests/test_net.c` (các hàm test client), `tests/test_net_session.c`, `tests/test_bridge.c`

**Interfaces:** mọi `client_*`, `ns_*`, `bridge_*` public và field `Client.on_pending_cb/pending_ctx` giữ nguyên.

- [ ] **Step 1: `tcp_client.c`.** `lookup_first → lookupFirstAddress`. Trong `client_connect`: `port_str → portText`, `res → addressList`, `p → candidate`, `fd → connectionSocket`, `ret → connectResult`, `pr → pollResult`, `sock_err → socketError`, `c → client`; `3000` → `enum { CONNECT_TIMEOUT_MS = 3000 };`. Trong `client_connect_start`: `fd_out → outFd`, `in_progress → outInProgress` (đổi cả tên tham số ở `.h`). Thêm FIXME cho `fcntl` bị bỏ qua trong `client_connect`. Comment: `connect` non-blocking trả `EINPROGRESS`, kết quả thật đọc qua `SO_ERROR`; `client_resolve` thử numeric trước để IP không chạm DNS.

- [ ] **Step 2: `net_session.c`.** 10 hàm static (`apply_js_response → applyJsResponse`, `clear_token → clearToken`, `resume_conn_state → resumeConnState`, …), `js_id → jsId`, `wire_id → wireId`, `conn_state → connState`. Comment `net_session.h`: backoff 1/2/4/8 s cap 8 s, không replay action cũ khi RESUME, token chỉ nằm trong `NetSession`, không bao giờ vào payload gửi JS.

- [ ] **Step 3: `bridge.c`.** `g_assets_dir/g_done_cb/g_done_ctx/g_disconnect_cb/g_disconnect_ctx/g_scheme_registered → gAssetsDir/gDoneCallback/gDoneContext/gDisconnectCallback/gDisconnectContext/gSchemeRegistered`. 7 hàm static (`hex_val → hexValue`, `url_decode → urlDecode`, `is_whitelisted_type → isWhitelistedType`, …), `rel_path → relativePath`, `js_id_out → outJsId`. `DISCONNECTED_MESSAGE`, `REJECTED_MESSAGE` và whitelist type giữ byte. Comment: vì sao chặn `..` trong `bridge_resolve_path`, whitelist type nào JS được gửi.

- [ ] **Step 4: `main.c`.** 16 biến `g_*` → `gXxx` (`g_client → gClient`, `g_last_rx_ms → gLastReceiveMs`, `g_connect_fd → gConnectFd`, …). Hàm static phần mạng: `attach_client → attachClient`, `start_connect_attempt → startConnectAttempt`, `on_heartbeat → onHeartbeat`, `schedule_reconnect → scheduleReconnect`, `handle_disconnect → handleDisconnect`, `send_resume → sendResume`, `now_ms → monotonicNowMs`, … Hàm UI (`on_decide_policy`, `on_load_changed`, `on_window_destroy`) và khối dựng cửa sổ trong `main()` giữ nguyên theo Global Constraints. Comment ở `onHeartbeat`: PING 5000 ms, coi link chết sau 15000 ms không nhận gì.

- [ ] **Step 5: Format; đổi tên hàm test trong `test_net.c` (phần client), `test_net_session.c`, `test_bridge.c`.**

- [ ] **Step 6: Chạy test.**

Run: `unoSync && make -B server && make test NAME=net && make test NAME=net_session && make integration NAME=transport`
Expected: exit 0.

Nếu WSL có GTK/WebKit: `make -B desktop && make test NAME=bridge && make desktop-smoke` (exit 0, in `desktop-smoke: OK`). Nếu không, ghi "bridge/desktop chờ kiểm trên Ubuntu 24.04" vào PR.

- [ ] **Step 7: Commit.**

```bash
git add desktop/ tests/test_net.c tests/test_net_session.c tests/test_bridge.c
git commit -m "refactor(c): apply conventions to desktop network session and bridge"
```

---

### Task 8: Kiểm tra cuối Task 2 và bàn giao cho Task 3

**Files:** không sửa code. Cập nhật mô tả PR.

- [ ] **Step 1: Chạy toàn bộ.**

Run:
```bash
unoSync && make -B server && for name in protocol net accounts rooms queue rating social store net_session bridge; do make test NAME="$name" || exit 1; done && make integration NAME=transport && make integration && make BUILD=build/refactor-headless HAVE_GTK= server test && make sanitize
```
Expected: exit 0. So với baseline Task 0 cùng số ca.

- [ ] **Step 2: Soát tên snake_case còn sót trong phần nội bộ.**

Run:
```bash
for f in shared/*.c server/main.c server/app.c server/accounts.c server/auth_worker.c server/rooms.c server/queue.c server/rating.c server/social.c server/store.c desktop/*.c; do grep -nE '^static [^=;(]*\b[a-z]+(_[a-z0-9]+)+\s*\(' "$f"; done; grep -nE '^\S.*\)\s*$' shared/*.c server/*.c desktop/*.c tests/test_*.c | grep -v ';'
```
Expected: lệnh thứ nhất chỉ còn các hàm UI đã liệt kê ở `desktop/main.c`. Lệnh thứ hai không còn hàm nào đặt `{` ở dòng sau.

- [ ] **Step 3: Soát diff.**

Run: `git diff --check main...HEAD && git diff --stat main...HEAD -- server/game tests/unit Makefile client`
Expected: `--check` không in gì; `--stat` rỗng (không chạm file của Bách, Makefile, client).

- [ ] **Step 4: Lập danh sách tên public còn snake_case cho Task 3.**

Run: `grep -ohE '\b[a-z]+(_[a-z0-9]+)+\(' shared/*.h server/app.h server/accounts.h server/auth_worker.h server/rooms.h server/queue.h server/rating.h server/social.h server/store.h desktop/*.h | sort -u`
Dán kết quả cùng các field snake_case trong header (`Conn.out_*`, `App.*`, `AppConnSlot.*`, `Session.*`, `RoomSeat.*`, `Client.on_pending_cb/pending_ctx`) vào PR làm đầu vào bảng cũ → mới của Task 3. Đánh dấu tên mà `server/game/*` gọi (`store_*`, `rating_*`, `rooms_*`, `app_*`) để Bách review hunk cross-owner.

- [ ] **Step 5: Mở PR `refactor/son-task2-convention → main`**, mô tả gồm baseline, kết quả Step 1, danh sách Step 4 và ghi chú bridge/desktop nếu chưa kiểm trên GTK. Nhờ Bách review framing (Task 1), command/push (Task 2) và seam native ↔ JS (Task 7). Mô tả PR không có dòng `Co-Authored-By`.
