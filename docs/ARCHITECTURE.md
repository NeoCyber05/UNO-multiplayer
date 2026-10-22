# UNO Multiplayer — Tài liệu Kiến trúc Backend (C / Linux)

> Phạm vi: Authoritative Game Server viết bằng C11 + POSIX trên Linux (WSL2), phục vụ
> client React hiện có (`client/`). Tài liệu này là bản thiết kế để code và để bảo vệ
> đồ án môn Lập trình mạng.

---

## 0. Tóm tắt quyết định (TL;DR)

| Vấn đề | Quyết định |
|---|---|
| Cầu nối Browser ↔ C | **Server C tự cài đặt WebSocket (RFC 6455) từ đầu** trên POSIX socket (tự viết SHA-1, Base64, HTTP parser, WS framing). Server cũng **phục vụ luôn file tĩnh của React build** trên cùng cổng. Một cổng TCP thứ hai nhận **raw TCP** cho client dòng lệnh `uno_cli` viết bằng C. |
| Mô hình đồng thời | **Reactor đơn luồng: `epoll` (level-triggered) + socket non-blocking + timer heap + `signalfd`**. Toàn bộ trạng thái game chỉ được chạm bởi 1 luồng → **không có race condition, không cần mutex**. Tuỳ chọn 1 worker thread cho I/O đĩa (SQLite), giao tiếp qua hàng đợi + `eventfd`. |
| Giao thức ứng dụng | **Giao thức nhị phân tự định nghĩa** "UNOP/1": header cố định 12 byte (magic, version, opcode, seq, length) + payload serialize thủ công theo network byte order. Framing bằng **length-prefix** trên luồng byte. Cùng một giao thức chạy trên cả raw TCP lẫn bên trong WebSocket. |
| Liveness | 3 lớp: phát hiện ở tầng socket (`read()==0`, `ECONNRESET`, `EPOLLRDHUP`, `EPIPE`) + **heartbeat ứng dụng PING/PONG 5s, timeout 15s** + `SO_KEEPALIVE` của kernel. Reconnect bằng **session token** 128-bit, giữ ghế 60s, bot đánh thay khi offline. |
| Luật chơi | Port `client/src/game/engine.js` sang C thành module **thuần (pure), không I/O**, unit-test được. Server là nguồn sự thật duy nhất; client chỉ gửi *ý định* (intent). |
| Timer | Turn timeout 20s và UNO grace 3s chạy **ở server** bằng min-heap timer + `CLOCK_MONOTONIC`, chống timer "ma" bằng **generation guard** (`turn_no`, `uno_watch_id`). |
| Fog of War | Hàm `view_build(game, viewer_seat)` dựng snapshot **riêng cho từng người xem**: chỉ chứa bài của chính mình (+ đồng đội ở 2v2 / Side to Side). Thứ tự chồng bài rút không bao giờ rời khỏi server. |

---

## 1. Tổng quan kiến trúc hệ thống

### 1.1 Sơ đồ khối

```
 ┌──────────────────────────── Máy client (LAN / VPN) ─────────────────────────────┐
 │                                                                                 │
 │  Browser: React + Vite build             Terminal: uno_cli (C, raw TCP)         │
 │  ┌───────────────────────────┐           ┌───────────────────────────┐          │
 │  │ UI (pages/, components/)  │           │ stdin commands / ncurses  │          │
 │  │   ▲ state       │ actions │           └─────────────┬─────────────┘          │
 │  │ useRemoteGame (hook)      │                         │                        │
 │  │ net/codec.js  (UNOP/1)    │                         │                        │
 │  │ net/connection.js (WS)    │                         │                        │
 │  └─────────┬─────────────────┘                         │                        │
 └────────────┼───────────────────────────────────────────┼────────────────────────┘
              │ HTTP GET (static) + WebSocket (binary)    │ TCP thuần
              │ ws://host:8080/ws                          │ host:9090
 ═════════════╪═══════════════════════════════════════════╪════════════════════════
              ▼                                           ▼
 ┌──────────────────────────── unod (C11, Linux, 1 tiến trình) ────────────────────┐
 │                                                                                 │
 │  ┌──────────── CORE: Reactor (1 thread) ───────────────────────────────────┐    │
 │  │ epoll_wait ◄── listen fds, client fds, signalfd, eventfd(worker)       │    │
 │  │ timer min-heap (CLOCK_MONOTONIC) → timeout của epoll_wait               │    │
 │  └───────┬──────────────────────────────────────────────────────────┬──────┘    │
 │          ▼                                                          ▼           │
 │  ┌──────── NET ─────────────────────────────┐        ┌───── TIMERS ─────────┐   │
 │  │ listener (accept4, setsockopt)           │        │ turn timeout 20s     │   │
 │  │ conn: in/out buffer, backpressure        │        │ UNO grace 3s         │   │
 │  │ transport_tcp  │ transport_ws │ http_static│       │ heartbeat sweep 1s   │   │
 │  │  (byte stream) │ (RFC 6455)   │ (GET+sendfile)     │ bot think delay      │   │
 │  └───────┬──────────────────────────────────┘        │ reconnect grace 60s  │   │
 │          ▼ luồng byte ứng dụng                       └──────────┬───────────┘   │
 │  ┌──────── PROTO ───────────────────────────┐                   │               │
 │  │ framer: tách gói theo header 12 byte     │                   │               │
 │  │ msg: encode/decode payload (wire.c)      │                   │               │
 │  └───────┬──────────────────────────────────┘                   │               │
 │          ▼ message đã parse (struct)                            │               │
 │  ┌──────── APP ─────────────────────────────────────────────────▼───────────┐   │
 │  │ dispatcher → session (login/resume) → lobby (matchmaking) → room          │   │
 │  │ room: ghế, host, bot, vòng đời ván; gọi engine; broadcast view            │   │
 │  │ view: Fog of War — dựng snapshot riêng cho từng ghế                       │   │
 │  └───────┬───────────────────────────────────────────────────────────────────┘   │
 │          ▼ action_t                       ▲ events + effect flags              │
 │  ┌──────── GAME (thuần, không I/O, không malloc khi chơi) ───────────────────┐   │
 │  │ card · deck (Fisher–Yates, CSPRNG seed) · rules · engine · bot            │   │
 │  └───────────────────────────────────────────────────────────────────────────┘   │
 │                                                                                 │
 │  [tuỳ chọn] worker thread ── job queue (mutex) ── SQLite (tài khoản, ELO)       │
 └─────────────────────────────────────────────────────────────────────────────────┘
```

**Quy tắc phụ thuộc (layering)** — chỉ được gọi xuống, không gọi ngược lên:

```
core  ←  net  ←  proto  ←  app  →  game
                                     (game không include bất cứ header nào của net/proto/app)
```

`game/` thuần tuý → test được bằng unit test không cần socket; đây cũng là điểm
mạnh khi bảo vệ: "tách biệt giao thức mạng khỏi logic nghiệp vụ".

### 1.2 Luồng hoạt động tổng thể

1. `unod` khởi động: đọc cấu hình (`getopt`), chặn `SIGINT/SIGTERM/SIGHUP` bằng
   `sigprocmask` và tạo `signalfd`, `signal(SIGPIPE, SIG_IGN)`, tạo 2 listening socket
   (8080 HTTP/WS, 9090 raw TCP), `epoll_create1(EPOLL_CLOEXEC)`.
2. Người chơi mở `http://<ip-server>:8080/` → server trả `index.html` + JS/CSS/ảnh của
   React build (`sendfile()`). Không cần Node trên máy demo.
3. React mở `ws://<ip-server>:8080/ws` → server nhận HTTP Upgrade, tính
   `Sec-WebSocket-Accept`, chuyển connection sang trạng thái WS.
4. Client gửi `HELLO` → `LOGIN{nick}` → nhận `LOGIN_OK{session_token}`.
5. Client vào hàng chờ (`QUEUE_JOIN{mode}`) hoặc tạo/vào phòng custom (`ROOM_CREATE`,
   `ROOM_JOIN{code}`). Lobby ghép đủ 4 ghế (người hoặc bot) → `ROOM_START`.
6. Room khởi tạo `game_t`, xào bài, chia 7 lá, gửi `GAME_STATE` đã lọc cho từng ghế,
   đặt turn timer 20s.
7. Mỗi hành động (`PLAY_CARD`, `DRAW_CARD`, `PASS`, `SAY_UNO`, `QUICK_CHAT`) đi
   qua: socket → framer → dispatcher → room → `game_apply()` → events → broadcast
   snapshot đã lọc + event cho hoạt ảnh.
8. Hết bài → `GAME_OVER{winner, team, stats, elo_delta}` → room về `FINISHED`,
   (tuỳ chọn) đẩy job ghi ELO cho worker thread.
9. `Ctrl+C` → `signalfd` báo `SIGINT` → server gửi `SERVER_SHUTDOWN` cho mọi client,
   flush buffer, `close()` toàn bộ fd, giải phóng bộ nhớ, thoát với mã 0 (valgrind sạch).

---

## 2. Bài toán 1 — Cầu nối Web Client (React) ↔ C Socket Server

Trình duyệt chỉ cho JS dùng HTTP(S), WebSocket, WebRTC, WebTransport — không có raw
TCP. Các hướng khả thi:

| # | Hướng | Mô tả | Ưu | Nhược | Điểm đồ án |
|---|---|---|---|---|---|
| A | **WebSocket tự cài đặt trong server C** | Server C nhận HTTP Upgrade, tự làm handshake (SHA-1 + Base64) và framing RFC 6455 trên POSIX socket | 100% backend là C; thể hiện hiểu biết sâu: parse HTTP trên luồng TCP, bit-level framing, masking, control frames; giữ nguyên UI React | Phải tự viết ~600 dòng (SHA-1, Base64, HTTP, WS) và test kỹ | ★★★★★ |
| B | Gateway/proxy (websockify, Node `ws`) dịch WS ↔ TCP | Browser ↔ WS ↔ proxy ↔ TCP ↔ server C | Server C chỉ cần TCP thuần, code ít | Thêm thành phần không phải C, thêm 1 hop độ trễ, điểm lỗi; hội đồng dễ hỏi "phần mạng thật nằm ở Node à?" | ★★★ |
| C | Đóng gói client bằng Electron/Tauri, dùng module `net` (Node) / Rust để mở raw TCP | Renderer React gọi IPC → main process mở socket TCP | Giao thức TCP thuần đầu-cuối; giữ UI React | App desktop nặng (~150MB), phải cài lên mọi máy demo, thêm tầng IPC; không còn "mở trình duyệt là chơi" | ★★★★ |
| D | Viết lại client bằng C (SDL2/raylib) | Native client dùng chung lib giao thức | Thuần C cả hai đầu | Bỏ toàn bộ UI hiện có; công sức UI rất lớn, khó đẹp bằng | ★★ |
| E | Dùng thư viện `libwebsockets` / `mongoose` | Thư viện C lo WS | Nhanh, ổn định | Che mất phần "lập trình mạng" mà môn học chấm (thường dùng event loop riêng của lib) | ★★ |
| F | HTTP long-polling / SSE | Client poll REST | Đơn giản | Không full-duplex, độ trễ cao, không hợp game realtime | ★ |

### 2.1 Đề xuất: **A + raw TCP song song ("dual transport")**

```
                ┌─ :8080 ─ HTTP GET  ──► http_static  (phục vụ client/dist)
 browser ───────┤
                └─ :8080 ─ GET /ws Upgrade ─► transport_ws ─┐
                                                            ├──► luồng byte ─► framer UNOP/1 ─► app
 uno_cli ──────── :9090 ─ TCP thuần ──────► transport_tcp ─┘
```

Lý do:

1. **Thuyết phục khi chấm:** mọi byte trên dây đều do code C của nhóm xử lý — từ
   `accept4()` tới parse HTTP, handshake, giải mask WebSocket, tới framing ứng dụng.
   Không có "hộp đen".
2. **Giữ nguyên UI đẹp hiện tại**: React chỉ thay `useUnoGame` bằng `useRemoteGame`
   trả về *cùng hình dạng dữ liệu*; các component `GameTable`, `MyHand`, `CenterTable`...
   không phải sửa.
3. **Raw TCP 9090 + `uno_cli`** chứng minh giao thức UNOP/1 độc lập transport — demo
   được bằng `uno_cli`, `nc`, Wireshark, và dùng làm công cụ stress test.
4. **Một binary, một lệnh chạy:** server phục vụ luôn file tĩnh → máy client chỉ cần
   trình duyệt, không cần Node/Vite khi demo.

Điểm mấu chốt về thiết kế: **WebSocket chỉ là "đường ống"**. Payload của các WS
binary frame được nối vào cùng một buffer luồng byte, rồi đi qua *đúng cùng một
framer* UNOP/1 như TCP thuần. Nhờ vậy giao thức tự định nghĩa vẫn là trung tâm, và
logic chống dính/cắt gói chỉ viết một lần.

### 2.2 Phần WebSocket cần tự cài đặt (RFC 6455, tập con đủ dùng)

**Handshake** (connection ở trạng thái `CONN_HTTP`):

- Đọc tới khi gặp `\r\n\r\n` (giới hạn 8 KiB, timeout 5s chống slowloris).
- Kiểm tra `GET /ws HTTP/1.1`, `Upgrade: websocket`, `Connection: Upgrade` (so khớp
  không phân biệt hoa thường, có thể là danh sách token), `Sec-WebSocket-Version: 13`,
  có `Sec-WebSocket-Key`.
- `accept = base64( sha1( key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )`.
- Trả `HTTP/1.1 101 Switching Protocols` + `Upgrade`, `Connection`,
  `Sec-WebSocket-Accept`. Chuyển trạng thái → `CONN_WS_OPEN`.
- Không phải `/ws` → đi nhánh `http_static` (chỉ `GET`/`HEAD`, chặn `..`, map MIME,
  `sendfile()`, `Connection: close`).

**Frame** (`transport_ws.c`):

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |   (16 nếu len=126, 64 nếu 127)|
|N|V|V|V|       |S|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|        Masking-key (32 bit, bắt buộc client→server)           |
+---------------------------------------------------------------+
|                         Payload Data                          |
+---------------------------------------------------------------+
```

- Client→server **bắt buộc có mask** → không có thì đóng với mã 1002.
- Unmask: `payload[i] ^= mask[i % 4]`.
- Opcode: `0x2` binary (nhận), `0x0` continuation (ghép fragment), `0x1` text (từ chối,
  1003), `0x8` close (trả close rồi đóng), `0x9` ping (trả `0xA` pong), `0xA` bỏ qua.
- RSV ≠ 0 → 1002 (không hỗ trợ extension `permessage-deflate`; handshake không đưa
  `Sec-WebSocket-Extensions` vào response).
- Giới hạn payload 64 KiB / frame và / message (chống cấp phát bộ nhớ quá lớn).
- Server→client: mỗi gói UNOP/1 gói vào **1 frame binary, FIN=1, không mask**.

Kiểm thử: unit test với vector chuẩn trong RFC (`dGhlIHNhbXBsZSBub25jZQ==` →
`s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`), test trên Chrome/Firefox thật.

### 2.3 Lưu ý triển khai WSL2 trong mạng LAN

WSL2 mặc định chạy sau NAT của Windows → máy khác trong LAN **không thấy** cổng của
WSL. Chọn một trong hai:

1. **Windows 11 mirrored networking** (khuyên dùng) — `%UserProfile%\.wslconfig`:
   ```ini
   [wsl2]
   networkingMode=mirrored
   ```
   Sau đó `wsl --shutdown`, mở firewall Windows cho TCP 8080, 9090.
2. **Port proxy**:
   ```powershell
   netsh interface portproxy add v4tov4 listenport=8080 listenaddress=0.0.0.0 connectport=8080 connectaddress=<IP-WSL>
   netsh advfirewall firewall add rule name="unod" dir=in action=allow protocol=TCP localport=8080,9090
   ```

Server luôn bind `0.0.0.0` (hoặc dual-stack `::` với `IPV6_V6ONLY=0`). Qua Internet:
Tailscale/ZeroTier (VPN mesh) hoặc tunnel (`cloudflared`, `ngrok tcp`).

---

## 3. Bài toán 2 — Mô hình đồng thời & I/O

### 3.1 Phân tích các phương án

| Mô hình | Cách làm | Đánh giá cho UNO (≤ vài trăm kết nối, turn-based, tải CPU rất thấp) |
|---|---|---|
| Thread-per-connection (blocking) | `accept` → `pthread_create`, mỗi thread `read()` chặn | Dễ viết lúc đầu, nhưng **4 thread cùng sửa 1 `game_t`** → cần mutex mỗi phòng + mutex lobby + mutex danh sách session; broadcast từ thread A ghi vào socket của B → phải khoá cả socket. Deadlock/race rất dễ xảy ra, timer phải có thêm thread. Tốn ~8 MiB stack ảo/thread. |
| Thread pool + hàng đợi | I/O thread đẩy job cho N worker | Vẫn phải khoá state theo phòng; thứ tự xử lý 2 gói liên tiếp của cùng người chơi có thể bị đảo nếu 2 worker lấy song song. Lợi ích chỉ có khi xử lý tốn CPU — game này mỗi action < 1 µs. |
| `select()` | Multiplexing cổ điển | Giới hạn `FD_SETSIZE`=1024, quét O(n), phải dựng lại fd_set mỗi vòng. Dùng được nhưng lỗi thời. |
| `poll()` | Mảng `pollfd` | Không giới hạn 1024 nhưng vẫn O(n) mỗi lần gọi, copy mảng vào kernel mỗi vòng. |
| **`epoll` Reactor đơn luồng** | 1 thread, non-blocking fd, `epoll_wait` trả về đúng fd sẵn sàng | O(số fd sẵn sàng); **trạng thái chỉ do 1 luồng sửa → tuần tự hoá tự nhiên, không lock**; timer, tín hiệu, sự kiện từ thread khác đều quy về fd (`signalfd`, `eventfd`) hoặc timeout của `epoll_wait`. Đây là mô hình của nginx, Redis, Node.js. |
| Reactor + worker pool (lai) | Reactor lo I/O, đẩy việc nặng sang pool | Chỉ đáng khi có việc nặng/blocking (DB, file). |

### 3.2 Quyết định

**Reactor đơn luồng trên `epoll`** làm lõi; **một worker thread tuỳ chọn** cho các việc
blocking (ghi SQLite tài khoản/ELO/lịch sử ván). Lập luận khi bảo vệ:

- Game turn-based: mỗi phòng tối đa vài action/giây; tổng throughput nhỏ hơn khả năng
  của 1 core hàng nghìn lần. Đa luồng không làm nhanh hơn mà chỉ thêm rủi ro.
- **Tính đúng đắn quan trọng hơn song song**: hai người cùng bấm `SAY_UNO`, timer UNO
  hết hạn đúng lúc người chơi đánh lá cuối — trong reactor, các sự kiện này được xử lý
  **lần lượt, nguyên tử**, không thể xen giữa nhau. Race condition bị loại bỏ *theo thiết
  kế*, không phải bằng khoá.
- Vẫn thể hiện đầy đủ kiến thức đồng thời: non-blocking I/O, multiplexing, event
  demultiplexing, backpressure, và (ở phần worker) mô hình producer–consumer với
  `pthread_mutex` + `eventfd` — đúng một chỗ khoá duy nhất, dễ chứng minh đúng.

**Level-triggered** (mặc định) thay vì edge-triggered: an toàn hơn (không mất sự kiện
nếu quên đọc cạn), vẫn đọc tới `EAGAIN` nhưng giới hạn 64 KiB/kết nối/vòng để công bằng
giữa các client. `EPOLLOUT` chỉ bật khi buffer gửi còn dữ liệu (tránh busy loop).

### 3.3 Vòng lặp sự kiện

```c
/* core/reactor.c — rút gọn */
while (!loop->stopping) {
    int timeout = timers_next_timeout_ms(&loop->timers, mono_ms()); /* -1 nếu rỗng */
    int n = epoll_wait(loop->epfd, evs, MAX_EVENTS, timeout);
    if (n < 0 && errno == EINTR) continue;
    loop->now = mono_ms();

    for (int i = 0; i < n; i++) {
        handler_t *h = evs[i].data.ptr;          /* listener | conn | signalfd | eventfd */
        h->on_event(h, evs[i].events);
    }
    timers_run_expired(&loop->timers, loop->now); /* turn timeout, UNO grace, bot, heartbeat */
    conns_reap_closed(loop);                      /* free trễ, tránh use-after-free */
}
```

`conns_reap_closed`: khi một connection lỗi giữa chừng (ví dụ `EPIPE` trong lúc đang
broadcast cho cả phòng), ta chỉ **đánh dấu `CLOSING`** và đưa vào danh sách chờ; việc
`epoll_ctl(DEL)`, `close()`, `free()` làm ở cuối vòng. Nhờ vậy không con trỏ nào bị
free trong lúc đang duyệt danh sách người chơi.

### 3.4 Luồng dữ liệu: từ byte trên socket tới broadcast

Ví dụ người chơi ghế 1 đánh lá `+4` chọn màu xanh:

```
 Client(seat 1)                        unod (1 thread)
 ─────────────                         ─────────────────────────────────────────────────
 PLAY_CARD{card=107,color=BLUE} ──►  [1] epoll_wait: fd#12 EPOLLIN
                                     [2] conn_on_readable: read() tới EAGAIN → conn->in
                                     [3] transport_ws_decode: tách WS frame, unmask
                                         → nối payload vào conn->stream
                                     [4] framer_next(): đủ 12 byte header? đủ length?
                                         → có 1 gói hoàn chỉnh (có thể còn dư nửa gói sau)
                                     [5] msg_decode(PLAY_CARD) → struct, kiểm tra biên
                                     [6] dispatcher: session đã login? đang trong room?
                                     [7] room_on_action(room, seat=1, action)
                                     [8] game_apply(&room->game, &action, events, &fx)
                                           ├─ đúng lượt? có lá 107 trên tay? hợp lệ với
                                           │  lá trên cùng/màu hiện tại? wild có màu?
                                           ├─ sai → trả mã lỗi, state KHÔNG đổi
                                           └─ đúng → cập nhật state, sinh events:
                                              CARD_PLAYED, DREW(seat 2, 4), SKIPPED(seat 2)
                                              fx = TURN_CHANGED | UNO_WATCH?
                                     [9] room áp effect: huỷ/đặt lại turn timer 20s
                                         (generation = turn_no), đặt UNO timer 3s nếu cần
                                    [10] room_broadcast: for seat in 0..3:
                                           view_build(game, seat) → msg_encode
                                           → conn_send(): ghi ngay nếu được,
                                             phần dư vào conn->out + bật EPOLLOUT
 ◄── ACK(seq) / ERROR(seq, code)
 ◄── GAME_EVENT + GAME_STATE (view riêng của ghế 1)       (ghế 0,2,3 nhận view riêng của họ)
```

Bước [2]–[10] chạy liền một mạch trong 1 thread, nên không thể có action nào khác chen
vào giữa lúc state đang ở trạng thái dở dang.

### 3.5 Worker thread (tuỳ chọn, giai đoạn sau)

```
 reactor ──push(job)──► [mutex + queue] ──► worker: sqlite3_exec(...)
    ▲                                           │
    └──── epoll ◄── eventfd ◄──push(result)─────┘
```

Worker **không bao giờ** chạm `game_t`/`room_t`; nó chỉ nhận bản sao dữ liệu (ví dụ
`{user_id, elo_delta}`) và trả kết quả qua hàng đợi kết quả + ghi 8 byte vào `eventfd`
để đánh thức reactor.

---

## 4. Bài toán 3 — Giao thức tầng ứng dụng "UNOP/1"

### 4.1 Định dạng gói

Mọi số nguyên là **big-endian (network byte order)**, encode/decode thủ công từng
trường (`wire_put_u16()`/`wire_get_u32()` dùng `htons/ntohl`) — **không bao giờ
`write()` nguyên một `struct`** (padding và endianness phụ thuộc trình biên dịch/kiến
trúc).

```
 0       1       2       3       4       5       6       7       8      ...     11      12
 ├───────┴───────┼───────┼───────┼───────────────────────────────┼───────────────────────┼────────────
 │  magic 0x554E │  ver  │ type  │            seq (u32)          │      length (u32)     │ payload...
 │     "UN"      │  =1   │opcode │  client tăng dần; server trả  │   số byte payload     │ (length byte)
 │               │       │       │  lại seq trong ACK/ERROR      │   0 … 16384           │
 └───────────────┴───────┴───────┴───────────────────────────────┴───────────────────────┴────────────
                         HEADER cố định 12 byte
```

- `magic` giúp phát hiện lệch khung / client lạ ngay lập tức (lệch → đóng kết nối, vì
  TCP không thể "đồng bộ lại" an toàn).
- `ver` cho phép mở rộng giao thức sau này.
- `seq`: client đánh số request; server trả `ACK{seq}` hoặc `ERROR{seq, code}` → client
  biết request nào bị từ chối (ví dụ để rung lá bài không hợp lệ). Gói server chủ động
  đẩy dùng `seq = 0`.
- `MAX_PAYLOAD = 16 KiB` — vượt → `ERROR(E_TOO_LARGE)` rồi đóng.

**Kiểu dữ liệu trong payload:**

| Kiểu | Mã hoá |
|---|---|
| `u8/u16/u32` | big-endian |
| `str` | `u8 len` + `len` byte UTF-8 (không có `\0`); server kiểm tra độ dài tối đa từng trường |
| `card` | `u8` card id 0..111 (xem bên dưới) |
| `list<T>` | `u8 count` + `count` phần tử |
| `token` | 16 byte thô |

**Mã hoá lá bài — 1 byte.** Bộ bài cố định 112 lá (khớp `deck.js`): mỗi màu `0`×1,
`1–9`×2, `skip/reverse/draw2`×2, `discard_all`×1 (= 26 lá × 4 màu = 104) + 4 `wild` +
4 `wild4`. Card id 0..111 tra bảng tĩnh `CARD_TABLE[id] = {color, value}` giống hệt ở
C và JS → chỉ cần gửi id. Màu: `0=red 1=yellow 2=green 3=blue 4=wild`; giá trị:
`0–9`, `10=skip 11=reverse 12=draw2 13=discard_all 14=wild 15=wild4`.

### 4.2 Danh mục message

| Nhóm | Opcode | Tên | Chiều | Payload |
|---|---|---|---|---|
| Hệ thống | `0x01` | `HELLO` | C→S | `u8 proto_ver, str client_name` |
| | `0x02` | `HELLO_OK` | S→C | `u16 heartbeat_ms, u16 timeout_ms` |
| | `0x03` | `PING` | S→C | `u32 server_ts` |
| | `0x04` | `PONG` | C→S | `u32 server_ts` (echo → đo RTT) |
| | `0x05` | `ACK` | S→C | (seq trong header) |
| | `0x06` | `ERROR` | S→C | `u16 code, str message` |
| | `0x07` | `BYE` / `SERVER_SHUTDOWN` | 2 chiều | `u8 reason` |
| Phiên | `0x10` | `LOGIN` | C→S | `str nick` (giai đoạn 2: `+ str password`) |
| | `0x11` | `LOGIN_OK` | S→C | `u32 user_id, token session, u16 elo` |
| | `0x12` | `RESUME` | C→S | `token session` |
| | `0x13` | `RESUME_OK` | S→C | `u8 where` (lobby/room/game) — sau đó server đẩy snapshot đầy đủ |
| Sảnh | `0x20` | `QUEUE_JOIN` | C→S | `u8 mode` |
| | `0x21` | `QUEUE_LEAVE` | C→S | — |
| | `0x22` | `QUEUE_STATUS` | S→C | `u8 mode, u8 waiting, u16 elapsed_s` |
| | `0x23` | `MATCH_FOUND` | S→C | `u32 room_id` |
| Phòng | `0x30` | `ROOM_CREATE` | C→S | `u8 mode, u8 flags (ranked?)` |
| | `0x31` | `ROOM_JOIN` | C→S | `str code` (6 ký tự) |
| | `0x32` | `ROOM_LEAVE` | C→S | — |
| | `0x33` | `ROOM_ADD_BOT` | C→S | `u8 seat, u8 level` (host) |
| | `0x34` | `ROOM_KICK` | C→S | `u8 seat` (host) |
| | `0x35` | `ROOM_SET_SEAT` | C→S | `u8 seat` (đổi ghế/đội) |
| | `0x36` | `ROOM_START` | C→S | — (host, đủ 4 ghế) |
| | `0x37` | `ROOM_STATE` | S→C | `u32 id, str code, u8 mode, u8 host_seat, 4×{u8 kind(empty/human/bot), u32 user_id, str nick, u8 team, u8 online}` |
| Game C→S | `0x40` | `PLAY_CARD` | C→S | `card id, u8 color` (color chỉ dùng với wild) |
| | `0x41` | `DRAW_CARD` | C→S | — |
| | `0x42` | `PASS` | C→S | — (chỉ hợp lệ sau khi vừa rút được lá đánh được) |
| | `0x43` | `SAY_UNO` | C→S | — |
| | `0x44` | `QUICK_CHAT` | C→S | `u8 phrase_id` (chat nhanh = id cố định, không gửi text tự do → khỏi lọc nội dung) |
| Game S→C | `0x50` | `GAME_STATE` | S→C | snapshot đã lọc (4.3) |
| | `0x51` | `GAME_EVENT` | S→C | `u8 kind, u8 actor, u8 target, card, u8 count, u8 color` — phục vụ hoạt ảnh/toast |
| | `0x52` | `CHAT_MSG` | S→C | `u8 seat, u8 phrase_id` |
| | `0x53` | `GAME_OVER` | S→C | `u8 winner_seat, u8 team, 4×{stats}, 4×{i16 elo_delta}` |

Mã lỗi (`u16`): `E_BAD_PACKET, E_TOO_LARGE, E_NOT_LOGGED_IN, E_NOT_IN_ROOM,
E_ROOM_FULL, E_ROOM_NOT_FOUND, E_NOT_HOST, E_NOT_YOUR_TURN, E_CARD_NOT_IN_HAND,
E_ILLEGAL_CARD, E_MUST_PLAY_DRAWN, E_NEED_COLOR, E_RATE_LIMIT, E_SESSION_EXPIRED`.

### 4.3 Snapshot `GAME_STATE` (theo góc nhìn từng ghế)

```
u32 state_version        // tăng mỗi lần state đổi → client bỏ gói cũ
u8  mode, u8 your_seat
u8  turn, i8 direction, u32 turn_no
u16 turn_ms_left         // thời gian còn lại, KHÔNG gửi mốc giờ tuyệt đối (đồng hồ 2 máy lệch nhau)
card top_card, u8 current_color, u8 discard_count, u8 draw_count
u8  flags                // bit0 has_drawn, bit1 uno_watch_active, bit2 game_over
card drawn_card          // 0xFF nếu không có (chỉ ý nghĩa với chính người rút)
4 × {
    u8 hand_count
    u8 said_uno, u8 online, u8 team
    list<card> hand      // RỖNG nếu người xem không có quyền thấy (Fog of War)
}
list<card> playable      // các lá người xem đang được phép đánh → UI làm sáng lá
```

Kích thước điển hình ~ 60–120 byte. Vì nhỏ, chiến lược là **mỗi lần state đổi gửi
snapshot đầy đủ** (đã lọc) thay vì delta: không bao giờ lệch trạng thái, reconnect chỉ
cần gửi lại 1 snapshot. `GAME_EVENT` chỉ là "gợi ý hiển thị" (bay lá bài, toast
"Đảo chiều!"), mất cũng không sai state.

### 4.4 Chống dính gói / cắt gói trên luồng TCP

TCP là **luồng byte**, không có ranh giới message: một `write()` 40 byte phía client có
thể đến server thành 2 lần `read()` (17 + 23), hoặc 3 message nhỏ dính vào 1 lần
`read()`. Giải pháp: **length-prefix framing + buffer tích luỹ theo từng kết nối**.

```c
/* proto/framer.c */
typedef enum { FR_NEED_MORE, FR_FRAME, FR_ERROR } fr_result_t;

fr_result_t framer_next(buf_t *in, frame_t *out) {
    if (buf_len(in) < HDR_SIZE) return FR_NEED_MORE;              /* cắt gói: chưa đủ header */

    const uint8_t *p = buf_data(in);
    if (rd_u16(p) != UNOP_MAGIC || p[2] != UNOP_VERSION) return FR_ERROR;
    uint32_t len = rd_u32(p + 8);
    if (len > MAX_PAYLOAD) return FR_ERROR;                       /* chống cấp phát vô hạn */

    if (buf_len(in) < HDR_SIZE + len) return FR_NEED_MORE;        /* cắt gói: chưa đủ payload */

    out->type = p[3];
    out->seq  = rd_u32(p + 4);
    out->len  = len;
    memcpy(out->payload, p + HDR_SIZE, len);
    buf_consume(in, HDR_SIZE + len);                              /* dính gói: phần dư giữ lại */
    return FR_FRAME;
}

/* conn.c — gọi sau mỗi lần read */
for (;;) {
    fr_result_t r = framer_next(&c->stream, &f);
    if (r == FR_NEED_MORE) break;
    if (r == FR_ERROR)     { conn_fail(c, E_BAD_PACKET); break; }
    dispatch(c, &f);
    if (c->state == CONN_CLOSING) break;
}
```

Buffer nhận: mảng tuyến tính có `head/tail`; khi `head` vượt nửa dung lượng thì
`memmove` phần còn lại về đầu (đơn giản, đủ nhanh với gói nhỏ). Dung lượng tối đa
`HDR_SIZE + MAX_PAYLOAD` → client gửi rác không làm phình bộ nhớ.

**Phía gửi** cũng có vấn đề tương tự: `write()` trên socket non-blocking có thể ghi
**một phần** hoặc trả `EAGAIN`. `conn_send()` nối dữ liệu vào `conn->out`, thử ghi
ngay; còn dư thì bật `EPOLLOUT`, ghi tiếp khi socket sẵn sàng, ghi hết thì tắt
`EPOLLOUT`. **Backpressure:** nếu `conn->out` vượt 256 KiB (client treo/mạng tắc),
coi là client chết → đóng, tránh để 1 client chậm làm phình RAM server.

Client JS làm y hệt (cùng thuật toán trên `Uint8Array` + `DataView`); dù WebSocket
đã bảo toàn ranh giới message, codec JS vẫn dùng framer luồng để đối xứng và để một
WS message có thể chứa nhiều gói UNOP.

Các tuỳ chọn socket: `TCP_NODELAY` (tắt Nagle — gói nhỏ, cần độ trễ thấp),
`SO_REUSEADDR` trên listener (restart server không bị `EADDRINUSE` vì `TIME_WAIT`),
`accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` và `accept` trong vòng lặp tới `EAGAIN`.

### 4.5 Heartbeat, phát hiện mất kết nối, reconnect

**Phát hiện mất kết nối — 3 lớp:**

| Lớp | Tín hiệu | Phát hiện được |
|---|---|---|
| Socket | `read()` trả 0 (FIN), `-1/ECONNRESET`, `EPOLLHUP`, `EPOLLRDHUP`, `write()`→`EPIPE` | Client đóng tab, process bị kill (kernel phía client gửi FIN/RST) |
| Ứng dụng | Server gửi `PING` mỗi 5s; mỗi gói nhận được cập nhật `last_rx_ms`; timer quét 1s/lần: `now - last_rx_ms > 15s` → đóng | **Half-open**: rút dây mạng, Wi-Fi rớt, máy client treo/ngủ — trường hợp không có FIN/RST nào tới server |
| Kernel | `SO_KEEPALIVE`, `TCP_KEEPIDLE=30`, `TCP_KEEPINTVL=10`, `TCP_KEEPCNT=3` | Lưới an toàn cho kết nối chưa login/không có heartbeat |

Client cũng tự giám sát: không nhận được gì trong 15s → coi như mất kết nối, đóng và
reconnect. `PONG` echo `server_ts` → server tính RTT, có thể hiển thị chỉ số ping trên UI.

**Tín hiệu OS:**

- `SIGPIPE`: `signal(SIGPIPE, SIG_IGN)` **và** gửi bằng `send(..., MSG_NOSIGNAL)`
  → ghi vào socket đã bị đóng chỉ trả `EPIPE`, không giết tiến trình.
- `SIGINT`, `SIGTERM`: chặn bằng `sigprocmask`, nhận qua `signalfd` trong epoll →
  graceful shutdown chạy trong luồng chính (không phải viết code trong signal handler,
  tránh vấn đề async-signal-safe).
- `SIGHUP`: nạp lại cấu hình / mở lại file log.

**Session & reconnect:**

```
connection (fd, buffer — sống ngắn)  ──n:1──►  session (user, token, room, seat — sống dài)
```

- `LOGIN_OK` cấp `session_token` 16 byte ngẫu nhiên từ `getrandom()`; client lưu
  `sessionStorage`.
- Mất kết nối giữa ván: `session->conn = NULL`, ghế đánh dấu `OFFLINE`, broadcast
  `ROOM_STATE` (avatar xám). Session giữ **60s**.
  - Tới lượt ghế offline: chờ 3s rồi **bot đánh thay** (`bot_choose_move`, port từ
    `bot.js`) — ván không bị treo 20s mỗi vòng.
  - Quá 60s: session hết hạn, ghế chuyển thành bot vĩnh viễn; ván xếp hạng tính là
    bỏ cuộc.
- Client reconnect với backoff 0.5s → 1s → 2s → 4s (tối đa 8s): `HELLO` →
  `RESUME{token}`. Server tra bảng băm token → gắn session vào connection mới. Nếu
  connection cũ vẫn còn (half-open chưa bị phát hiện) thì **đóng connection cũ**
  (session takeover). Gửi `RESUME_OK` + `ROOM_STATE` + `GAME_STATE` đầy đủ.
- Đóng server chủ động (`SERVER_SHUTDOWN`) → client không reconnect.

---

## 5. Bài toán 4 — Authoritative Game Server & Luật chơi

### 5.1 Nguyên tắc

- Client gửi **ý định** (`PLAY_CARD 107 BLUE`), không gửi kết quả. Server kiểm tra, áp
  dụng, rồi gửi state mới. Client không tự tính luật — chỉ dùng `playable` từ server
  để làm sáng lá bài.
- Xào bài, rút bài, bộ đếm, timer, bot: tất cả ở server.
- RNG: Fisher–Yates với PRNG xoshiro256\*\* được seed từ `getrandom()` cho mỗi ván
  (không dùng `rand()`; có tuỳ chọn `--seed` để tái hiện ván khi debug).

### 5.2 Mô hình dữ liệu

```c
/* app/session.h */
typedef struct session {
    uint32_t     user_id;
    char         nick[MAX_NICK + 1];
    uint8_t      token[16];
    struct conn *conn;            /* NULL khi offline */
    struct room *room;
    int8_t       seat;            /* -1 khi không ngồi */
    uint64_t     offline_since_ms;
    uint16_t     elo;
} session_t;

/* app/room.h */
typedef enum { ROOM_WAITING, ROOM_PLAYING, ROOM_FINISHED } room_state_t;
typedef enum { SEAT_EMPTY, SEAT_HUMAN, SEAT_BOT } seat_kind_t;

typedef struct room {
    uint32_t      id;
    char          code[7];        /* mã mời 6 ký tự cho phòng custom */
    uint8_t       mode;           /* MODE_CLASSIC | MODE_2V2 | MODE_SIDE */
    bool          ranked;
    room_state_t  state;
    seat_kind_t   seat_kind[4];
    session_t    *seat_sess[4];
    uint8_t       bot_level[4];
    int8_t        host_seat;
    game_t        game;
    timer_id_t    turn_timer, uno_timer, bot_timer;
} room_t;

/* game/engine.h — dữ liệu thuần, kích thước cố định, không con trỏ */
typedef struct game {
    uint8_t  mode, n;                     /* n = 4 */
    uint8_t  hand[4][DECK_SIZE];  uint8_t hand_len[4];
    uint8_t  draw[DECK_SIZE];     uint8_t draw_len;
    uint8_t  discard[DECK_SIZE];  uint8_t discard_len;
    uint8_t  cur_color;
    int8_t   direction;                   /* +1 / -1 */
    uint8_t  turn;
    uint32_t turn_no;
    bool     has_drawn;   uint8_t drawn_card;
    bool     said_uno[4];
    int8_t   uno_watch;   uint32_t uno_watch_id;
    uint8_t  team[4];
    int8_t   winner;                      /* -1 khi chưa xong */
    player_stats_t stats[4];
    uint32_t version;
    rng_t    rng;
} game_t;
```

`game_t` không có con trỏ, kích thước cố định → có thể `memcpy` để snapshot/test,
không `malloc` trong lúc chơi.

### 5.3 API engine (thuần)

```c
typedef enum { ACT_PLAY, ACT_DRAW, ACT_PASS, ACT_SAY_UNO,
               ACT_TIMEOUT, ACT_UNO_PENALTY } act_type_t;

typedef struct {
    act_type_t type;
    uint8_t    seat, card, color;
    uint32_t   guard;     /* turn_no cho TIMEOUT, uno_watch_id cho UNO_PENALTY */
} action_t;

enum { FX_TURN_CHANGED = 1, FX_UNO_WATCH = 2, FX_UNO_CLEARED = 4, FX_GAME_OVER = 8 };

/* Trả 0 nếu hợp lệ (state đã đổi), hoặc mã lỗi E_* (state giữ nguyên). */
int game_apply(game_t *g, const action_t *a, event_t *ev, size_t *n_ev, unsigned *fx);

void game_init(game_t *g, uint8_t mode, uint64_t seed);
bool game_can_play(const game_t *g, uint8_t seat, uint8_t card);
size_t game_playable(const game_t *g, uint8_t seat, uint8_t *out);
```

### 5.4 Luật (port từ `engine.js`, giữ nguyên hành vi)

**Kiểm tra `PLAY_CARD`** theo thứ tự, sai ở bước nào trả mã lỗi bước đó:

1. Ván chưa kết thúc; `seat == turn` → `E_NOT_YOUR_TURN`.
2. Lá có trên tay → `E_CARD_NOT_IN_HAND`.
3. Nếu `has_drawn` thì chỉ được đánh đúng lá vừa rút → `E_MUST_PLAY_DRAWN`.
4. `wild`/`wild4` luôn hợp lệ; lá thường hợp lệ khi `color == cur_color` hoặc
   `value == top.value` → `E_ILLEGAL_CARD`.
5. Lá wild phải kèm màu 0..3 → `E_NEED_COLOR`. (Trạng thái `pendingWild` của bản JS
   là chuyện của UI: client mở `ColorPicker` rồi mới gửi một gói `PLAY_CARD` có màu.)

**Hiệu ứng lá** (`nextIndex = (from + direction*steps) mod n`):

| Lá | Hiệu ứng | Lượt tiếp |
|---|---|---|
| Số | — | +1 |
| `skip` | người kế mất lượt | +2 |
| `reverse` | `direction = -direction` | +1 theo chiều mới |
| `draw2` | người kế rút 2, mất lượt | +2 |
| `wild4` | người kế rút 4, mất lượt, đổi màu | +2 |
| `wild` | đổi màu | +1 |
| `discard_all` | bỏ luôn mọi lá cùng màu trên tay | +1 |

Hết bài sau khi đánh → `winner = seat`, ở chế độ đội thì đội của người đó thắng.
Chồng rút hết → xào lại phần `discard` trừ lá trên cùng (`giveCards`).

**`DRAW_CARD`**: rút 1 lá; nếu đánh được thì `has_drawn = true, drawn_card = id`
(người chơi chọn đánh lá đó hoặc `PASS`); không đánh được thì tự qua lượt.

### 5.5 Luật quên hô UNO (grace 3 giây)

```
PLAY_CARD làm tay còn 1 lá và said_uno == false
   → uno_watch = seat, uno_watch_id = ++counter, fx |= FX_UNO_WATCH
   → room đặt timer 3000ms với guard = uno_watch_id

SAY_UNO hợp lệ khi:
   (a) seat == uno_watch                       → huỷ watch, said_uno = true
   (b) tay còn 2 lá, đang tới lượt mình        → hô sớm, said_uno = true

Timer 3s nổ → action UNO_PENALTY{guard}
   → engine chỉ phạt nếu uno_watch_id == guard (watch chưa bị huỷ/thay thế)
   → rút 2 lá, said_uno = false, stats.penalties++
```

Mốc 3 giây được đo **tại server**, tính từ lúc server áp dụng lá bài; trong LAN độ trễ
vài ms nên không cần bù trừ. Vì reactor xử lý tuần tự, trường hợp `SAY_UNO` và timer
cùng "đến lúc" luôn có thứ tự xác định: cái nào được xử lý trước thắng, không có trạng
thái nửa vời.

### 5.6 Đếm ngược lượt 20 giây ở backend

```c
/* app/room.c */
static void room_arm_turn_timer(room_t *r) {
    timer_cancel(&loop->timers, r->turn_timer);
    uint32_t ms = seat_is_online(r, r->game.turn) ? TURN_MS : OFFLINE_TURN_MS;
    r->turn_deadline = loop->now + ms;
    r->turn_timer = timer_add(&loop->timers, ms, on_turn_timeout, r, r->game.turn_no);
}

static void on_turn_timeout(void *ctx, uint32_t gen) {
    room_t *r = ctx;
    action_t a = { .type = ACT_TIMEOUT, .seat = r->game.turn, .guard = gen };
    room_apply(r, &a);  /* engine bỏ qua nếu gen != turn_no (timer "ma") */
}
```

- **Generation guard**: dù timer cũ chưa kịp huỷ (hoặc huỷ lười), callback mang
  `turn_no` lúc tạo; engine so `guard != g->turn_no` → bỏ qua. Đây là cùng ý tưởng
  `a.turnNo !== s.turnNo` trong `engine.js`.
- Hết giờ: nếu chưa rút thì tự rút 1 lá, rồi qua lượt; broadcast state + event.
- Sau `DRAW_CARD` rút được lá đánh được, timer được đặt lại (giống `turnKey` của bản JS).
- Client nhận `turn_ms_left` và tự đếm ngược để vẽ `MatchClock`; đồng hồ client chỉ để
  hiển thị, quyết định cuối cùng là của server.
- **Timer heap**: min-heap theo `deadline` (`CLOCK_MONOTONIC`, không bị ảnh hưởng khi
  đổi giờ hệ thống); `timer_cancel` đánh dấu huỷ, phần tử bị bỏ khi nổi lên đỉnh.
  `epoll_wait` dùng `deadline_gần_nhất - now` làm timeout → không cần thread timer.
  (Phương án thay thế để trình bày: `timerfd_create` — mỗi timer là 1 fd trong epoll.)

### 5.7 Fog of War

```c
/* app/view.c */
static bool can_see_hand(const game_t *g, uint8_t viewer, uint8_t owner) {
    if (viewer == owner) return true;
    if (g->mode == MODE_CLASSIC) return false;
    return g->team[viewer] == g->team[owner];   /* 2v2 & Side to Side: đồng đội lật bài */
}

void view_build(const game_t *g, uint8_t viewer, game_view_t *out) {
    for (uint8_t s = 0; s < g->n; s++) {
        out->p[s].hand_count = g->hand_len[s];
        out->p[s].hand_len   = can_see_hand(g, viewer, s) ? g->hand_len[s] : 0;
        if (out->p[s].hand_len) memcpy(out->p[s].hand, g->hand[s], g->hand_len[s]);
    }
    out->drawn_card = (viewer == g->turn && g->has_drawn) ? g->drawn_card : CARD_NONE;
    /* draw pile: chỉ gửi SỐ LƯỢNG. Thứ tự chồng bài không bao giờ rời server. */
    ...
}
```

Xếp đội (khớp `MODES` trong `constants.js`): **2v2** đồng đội ngồi đối diện
(ghế 0–2 vs 1–3); **Side to Side** đồng đội ngồi cạnh (0–1 vs 2–3).

Các điểm rò rỉ thường bị bỏ sót, cần kiểm tra:

- `GAME_EVENT` khi đối thủ rút bài chỉ chứa **số lượng**, không chứa card id
  (`card = 0xFF`); riêng người rút (và đồng đội ở chế độ đội) nhận id.
- Lá bị `discard_all` bỏ xuống thì công khai (nằm trên chồng bỏ) — được gửi.
- Log server ở mức `INFO` không in bài trên tay; chỉ mức `TRACE` khi debug.
- Client không bao giờ nhận `game_t` gốc; mọi đường gửi state đều đi qua `view_build`.

### 5.8 Lobby, matchmaking, phòng

- **Matchmaking** (`lobby.c`): mỗi mode xếp hạng có một hàng đợi FIFO. Mỗi lần có người
  vào hàng hoặc timer 1s nổ: nếu đủ 4 người → tạo room; nếu người đầu hàng chờ quá
  30s → lấp chỗ trống bằng bot (giống trải nghiệm hiện tại). Có thể mở rộng ghép theo
  ELO (cửa sổ ±100, nới dần theo thời gian chờ).
- **Phòng custom** (`CustomRoom.jsx`/`TeamRoom.jsx`): host tạo phòng, nhận mã 6 ký tự
  (bảng chữ bỏ `0/O/1/I`); bạn bè `ROOM_JOIN{code}`; host thêm bot / kick / start.
  Host rời phòng → chuyển quyền host cho người tiếp theo; phòng trống → huỷ.
- Mọi thay đổi phòng → broadcast `ROOM_STATE` cho cả phòng.
- Chống spam: token bucket mỗi kết nối 20 gói/s, `QUICK_CHAT` 1 lần/1.5s; vượt →
  `E_RATE_LIMIT`, vượt nhiều lần → đóng.

---

## 6. Tích hợp phía client React

Không đổi giao diện; thêm tầng mạng và một hook có **cùng hình dạng trả về** với
`useUnoGame`:

```
client/src/net/
  cards.js          bảng CARD_TABLE (giống server, sinh ra từ cùng một nguồn)
  codec.js          encode/decode UNOP/1 bằng DataView (big-endian mặc định)
  framer.js         tách gói từ luồng byte (cùng thuật toán với framer.c)
  connection.js     WebSocket (binaryType='arraybuffer'), heartbeat watchdog,
                    reconnect backoff, RESUME bằng token trong sessionStorage
  NetContext.jsx    Provider: trạng thái kết nối, session, room
src/game/useRemoteGame.js
  → trả về { state, me, top, isMyTurn, playableIds, canSayUno, turnKey, actions }
    actions.play(cardId) gửi PLAY_CARD; ColorPicker gửi kèm màu; ...
```

`useRemoteGame` xoay ghế để người xem luôn ở index 0 (ghế dưới), vì UI hiện đặt
`players[0] = bạn` và `SEATS = ['bottom','left','top','right']`. `match.test.js` và
engine JS có thể giữ lại cho chế độ chơi offline với bot.

---

## 7. Cấu trúc mã nguồn & build

### 7.1 Cây thư mục

```
server/
├── Makefile
├── README.md                  cách build, chạy, demo
├── src/                       header .h đặt cạnh .c theo module
│   ├── main.c                 parse args, khởi tạo, chạy loop, shutdown
│   ├── config.c/.h            getopt: --http-port --tcp-port --www --seed --log-level
│   │
│   ├── core/                  ── không biết gì về UNO ──
│   │   ├── reactor.c/.h       epoll wrapper, handler_t, vòng lặp, deferred close
│   │   ├── timer.c/.h         min-heap timer, generation, CLOCK_MONOTONIC
│   │   ├── signals.c/.h       sigprocmask + signalfd, SIGPIPE ignore
│   │   ├── worker.c/.h        (tuỳ chọn) job queue + pthread + eventfd
│   │   └── log.c/.h           log có mức, timestamp, hex dump gói (--trace-wire)
│   │
│   ├── net/
│   │   ├── listener.c/.h      socket/bind/listen, SO_REUSEADDR, accept4 loop
│   │   ├── conn.c/.h          conn_t, đọc/ghi non-blocking, out buffer, backpressure
│   │   ├── buffer.c/.h        buf_t: append/consume/compact
│   │   ├── sockopt.c/.h       TCP_NODELAY, keepalive, nonblock
│   │   ├── http.c/.h          parser request tối thiểu (request line + headers)
│   │   ├── http_static.c/.h   phục vụ client/dist: MIME, chặn path traversal, sendfile
│   │   ├── ws.c/.h            handshake + encode/decode frame RFC 6455
│   │   └── transport.c/.h     trừu tượng: tcp | ws → luồng byte ứng dụng
│   │
│   ├── crypto/
│   │   ├── sha1.c/.h          cho Sec-WebSocket-Accept
│   │   ├── base64.c/.h
│   │   └── random.c/.h        getrandom() + xoshiro256**
│   │
│   ├── proto/
│   │   ├── opcodes.h          enum opcode, mã lỗi, hằng số giao thức
│   │   ├── wire.c/.h          put/get u8/u16/u32/str/list với kiểm tra biên
│   │   ├── framer.c/.h        header 12 byte, tách gói
│   │   └── msg.c/.h           struct cho từng message + encode/decode
│   │
│   ├── app/
│   │   ├── dispatcher.c/.h    bảng opcode → handler, kiểm tra trạng thái phiên
│   │   ├── session.c/.h       login, token, resume, hết hạn
│   │   ├── lobby.c/.h         hàng đợi matchmaking
│   │   ├── room.c/.h          vòng đời phòng, ghế, timer, gọi engine, broadcast
│   │   ├── view.c/.h          Fog of War, dựng snapshot theo ghế
│   │   └── ratelimit.c/.h     token bucket
│   │
│   ├── game/                  ── THUẦN: không include net/proto/app, không I/O ──
│   │   ├── card.c/.h          CARD_TABLE[112], màu/giá trị
│   │   ├── deck.c/.h          tạo bộ, xào Fisher–Yates, rút/xào lại
│   │   ├── rules.c/.h         can_play, next_index, playable
│   │   ├── engine.c/.h        game_init, game_apply
│   │   └── bot.c/.h           port từ client/src/game/bot.js
│   │
│   └── util/
│       ├── hashmap.c/.h       token → session, code → room
│       └── list.h             intrusive linked list
│
├── tests/
│   ├── test_main.c            runner tối giản (macro ASSERT, không cần lib)
│   ├── test_wire.c            encode/decode, giá trị biên
│   ├── test_framer.c          cắt 1 byte/lần, dính 50 gói, header hỏng, length quá lớn
│   ├── test_ws.c              vector handshake RFC, mask/unmask, length 126/127
│   ├── test_engine.c          port các kịch bản của client/src/game/match.test.js
│   └── test_view.c            Fog of War: classic không thấy, đội thấy đồng đội
│
├── tools/
│   ├── uno_cli.c              client raw TCP (chơi được qua terminal)
│   ├── stress.c               N client giả lập, mỗi client tự chơi bằng bot
│   ├── torture.c              gửi gói theo kiểu xé lẻ / dính / rác / ngắt giữa chừng
│   └── unop.lua               Wireshark dissector cho UNOP/1 (hiển thị gói khi demo)
│
└── www -> ../client/dist      symlink: build React được phục vụ tĩnh
```

### 7.2 Makefile

```make
CC       ?= gcc
TARGET   := unod
BUILD    ?= debug
SRC_DIRS := src src/core src/net src/crypto src/proto src/app src/game src/util
SRCS     := $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c))
OBJDIR   := build/$(BUILD)
OBJS     := $(SRCS:%.c=$(OBJDIR)/%.o)
DEPS     := $(OBJS:.o=.d)

CPPFLAGS := -Isrc -D_GNU_SOURCE            # epoll, signalfd, accept4, getrandom
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wnull-dereference \
            -Wcast-align -Wpointer-arith -Werror=implicit-function-declaration \
            -MMD -MP
LDFLAGS  :=
LDLIBS   := -lpthread                      # + -lsqlite3 khi bật persistence

ifeq ($(BUILD),debug)
  CFLAGS  += -O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
else ifeq ($(BUILD),release)
  CFLAGS  += -O2 -DNDEBUG -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
  LDFLAGS += -pie -Wl,-z,relro,-z,now
else ifeq ($(BUILD),valgrind)          # valgrind không chạy chung với ASan
  CFLAGS  += -O0 -g3
endif

.PHONY: all release test valgrind cli tools clean

all: $(OBJDIR)/$(TARGET)

$(OBJDIR)/$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

release:
	$(MAKE) BUILD=release

# Test link mọi module trừ main.c
LIB_OBJS  := $(filter-out $(OBJDIR)/src/main.o,$(OBJS))
TEST_SRCS := $(wildcard tests/*.c)
test: $(LIB_OBJS) $(TEST_SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(TEST_SRCS) $(LIB_OBJS) $(LDLIBS) -o $(OBJDIR)/run_tests
	./$(OBJDIR)/run_tests

valgrind:
	$(MAKE) BUILD=valgrind
	valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes \
	         --error-exitcode=1 ./build/valgrind/$(TARGET) --http-port 8080

tools: $(LIB_OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tools/uno_cli.c $(LIB_OBJS) $(LDLIBS) -o $(OBJDIR)/uno_cli
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tools/stress.c  $(LIB_OBJS) $(LDLIBS) -o $(OBJDIR)/stress
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tools/torture.c $(LIB_OBJS) $(LDLIBS) -o $(OBJDIR)/torture

clean:
	rm -rf build

-include $(DEPS)
```

Ghi chú:

- `-D_GNU_SOURCE`: cần cho `accept4`, `epoll_*`, `signalfd`, `getrandom` (API Linux).
  Phần còn lại tuân thủ POSIX.1-2008; nếu muốn chứng minh tính khả chuyển có thể thêm
  backend `poll()` sau cùng interface `reactor.h` (`-DUSE_POLL`), để so sánh khi bảo vệ.
- Bản debug bật ASan/UBSan — bắt use-after-free, tràn buffer trong parser ngay khi test.
- `make valgrind` với `--track-fds=yes` chứng minh không rò rỉ fd sau shutdown.
- Thêm `.clang-format` và `cppcheck --enable=all src/` vào CI nếu có.

---

## 8. Kiểm thử & kịch bản demo khi bảo vệ

| Mục tiêu | Cách chứng minh |
|---|---|
| Framing đúng | `torture` gửi gói xé lẻ từng byte có `usleep`, gửi 50 gói trong 1 `write()`, gửi header sai magic → server xử lý đúng/đóng đúng; `test_framer.c` |
| Giao thức tự định nghĩa | Wireshark + `unop.lua` hiển thị từng trường header/payload; `--trace-wire` in hex dump |
| WebSocket tự viết | Wireshark thấy HTTP 101 + WS binary frame; Chrome DevTools tab Network → WS |
| Đồng thời | `stress -n 200` (50 phòng × 4 client) chạy song song; `top` cho thấy 1 thread, CPU thấp |
| Mất kết nối đột ngột | `kill -9` client → server log `ECONNRESET`/FIN ngay; rút mạng/`iptables -j DROP` cổng client → heartbeat phát hiện sau ≤15s; ghế chuyển offline → bot đánh thay → bật lại mạng → `RESUME` vào lại đúng ván |
| Mạng xấu | `tc qdisc add dev eth0 root netem delay 200ms loss 5%` → game vẫn đúng (TCP lo mất gói, timer server vẫn công bằng) |
| Tín hiệu & dọn dẹp | `Ctrl+C` → client nhận `SERVER_SHUTDOWN`; `make valgrind`: 0 leak, 0 fd mở thừa; `ss -tanp` không còn socket |
| Anti-cheat / Fog of War | Dùng `uno_cli` gửi `PLAY_CARD` lá không có trên tay / không đúng lượt → `ERROR`; xem gói `GAME_STATE` trong Wireshark: không có bài đối thủ |
| Luật chơi | `test_engine.c` với seed cố định: Skip/Reverse/+2/+4/Discard All, xào lại chồng bài, phạt UNO, timeout |

---

## 9. Lộ trình triển khai

| Giai đoạn | Nội dung | Kết quả kiểm chứng |
|---|---|---|
| P0 | `core/` (reactor, timer, signals), `net/listener+conn+buffer`, `proto/framer+wire`, `uno_cli` gửi `HELLO/PING` qua TCP 9090 | `test_framer`, `torture` pass; Ctrl+C sạch |
| P1 | `crypto/sha1+base64`, `net/http+ws+http_static`; React kết nối `ws://.../ws`, heartbeat | Trình duyệt ở máy khác trong LAN load được UI từ server C, thấy ping |
| P2 | `game/` port từ JS + `test_engine` (seed cố định) | Mọi kịch bản `match.test.js` pass bằng C |
| P3 | `app/session+room+view+dispatcher`, phòng custom + bot, `useRemoteGame` | 4 trình duyệt chơi trọn 1 ván Classic |
| P4 | Matchmaking, 2v2/Side to Side, reconnect/RESUME, bot thay người offline, rate limit | Kịch bản rút mạng → vào lại đúng ván |
| P5 (tuỳ chọn) | Worker thread + SQLite (tài khoản, ELO, leaderboard), Wireshark dissector, `stress`, backend `poll()` để so sánh | Bảng xếp hạng thật; số liệu stress cho báo cáo |
