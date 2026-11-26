# Bàn giao Sơn → Bách: Task 2 refactor convention (26/11)

Nhánh `refactor/son-task2-convention`, tạo từ `main` @ `31f61ab`. Plan: [2026-11-26-son-task2-convention.md](plans/2026-11-26-son-task2-convention.md).

## Đã làm

- Chuẩn hóa format, tên nội bộ và comment cho `shared/`, `server/{main,app,accounts,auth_worker,rooms,queue,rating,social,store}.*`, `desktop/{tcp_client,net_session,bridge,main}.*` và 10 test C của Sơn.
- Chỉ đổi tên **nội bộ file**: hàm `static`, biến cục bộ, tham số (cả trong prototype của header), biến file-scope và field của struct định nghĩa trong `.c`. Không đổi tên hàm public, kiểu, enum hay field của struct trong header, nên `server/game/*` không cần sửa.
- Một thay đổi hành vi, nằm trong commit riêng `fix(net)`: `appRejectConnection()` (`server/app.h`) gửi frame `SERVER_FULL`, ghi stderr khi `send()` lỗi hoặc gửi thiếu, và luôn đóng socket. Có 2 test mới trong `tests/test_net.c`.
- Hằng số mới, giữ nguyên trị số: `LISTEN_BACKLOG = 128`, `POLL_TIMEOUT_MS = 50`, `SERVER_FULL_FRAME_SIZE = 256`, `RATE_TOKENS_PER_MS = 0.03`, `RATE_BURST_TOKENS = 60.0`, `RECEIVE_BUFFER_SIZE = 16384`, `CONNECT_TIMEOUT_MS = 3000` (tcp_client), `DEFAULT_SERVER_PORT = 9090`, các giới hạn validate trong `accounts.c`.
- Lỗi chưa xử lý được đánh dấu `FIXME` kèm hướng xử lý, không đổi hành vi: kết quả `fcntl(F_SETFL)`/`setsockopt` bị bỏ qua (`app.c`, `net.c`, `tcp_client.c`), `pipe()` thất bại thì không có auth worker, kết quả auth bị mất khi pipe đầy (`auth_worker.c`).

## Kiểm chứng

Chạy trong WSL Ubuntu-22.04 (có GTK/WebKit), từ bản sao LF của nhánh, so với baseline `e3374b9`:

| Lệnh | Baseline | Sau Task 2 |
|---|---|---|
| `make -B server desktop` | 0 | 0 |
| `make -B test` (16 test C) | 0 | 0 |
| `make integration` | 56/56 OK | 56/56 OK |
| `make BUILD=build/refactor-headless HAVE_GTK= server test` | — | 0, 15 test |
| `make sanitize` | 0 | 0 |
| `make desktop-smoke` | 0 | 0 |

Chưa chạy trên Ubuntu 24.04; nghiệm thu cuối theo Task 5 của plan tổng vẫn cần làm ở đó.

- Nội dung SQL: so `.rodata` của `store.o` giữa baseline và HEAD, giống hệt (292 chuỗi).
- Build với `-Wshadow`: 0 cảnh báo, bằng baseline. Lần kiểm này đã bắt được một lỗi đổi tên làm sai badge trong `applyProfileDelta` trước khi commit.

## Đầu vào cho Task 3 (bảng cũ → mới)

Hàm public còn snake_case trong header của Sơn:

- `shared/`: `frame_reader_reset`, `frame_feed`, `frame_encode`, `status_code`, `conn_init`, `conn_read`, `conn_queue`, `conn_flush`, `conn_close`
- `server/app.h`: `app_init`, `app_run`, `app_stop`, `app_cleanup`, `app_close_slot`, `app_user_leave`, `app_conn_check_rate_limit`, `app_now_ms`, `app_push`, `app_reply_status`, `app_reply_ok`
- `server/accounts.h`, `auth_worker.h`: `accounts_*` (9), `sessions_*` (8), `auth_drain`, `auth_submit`, `auth_worker_init`, `auth_worker_destroy`
- `server/rooms.h`, `queue.h`: `rooms_*` (20), `party_find_by_user`, `party_leave`, `queue_*` (11)
- `server/rating.h`, `social.h`, `store.h`: `rating_*` (4), `social_*` (5), `store_*` (18)
- `desktop/`: `client_*` (11), `ns_*` (11), `bridge_*` (11)

Field snake_case trong struct public:

- `Conn`: `out_buf`, `out_len`, `out_pos`
- `App`: `listen_fd`, `bound_port`, `bind_ip`, `db_path`, `auth_limit`, `auth_worker`, `auth_pipe_read`, `auth_pipe_write`, `rate_limiter`, `heartbeat_ms`, `match_records`, `leaving_user`
- `AppConnSlot`: `client_ip`, `rate_last_ms`, `rate_tokens`, `last_request_id`, `last_frame_ms`
- `SessionEntry`, `AuthJob`, `AuthResult`: `in_use`, `user_id`, `conn_id`, `created_at_ms`, `req_id`, `user_exists`
- `RoomSeat`, `Room`, `Invite`, `Party`, `Rooms`: `is_bot`, `user_id`, `bot_level`, `host_seat`, `invite_id`, `sender_id`, `recipient_id`, `room_code`, `created_at_ms`, `party_id`, `leader_id`, `member_count`, `on_start`, `next_party_id`, `next_invite_id`, `next_queue_room`
- `Client`: `on_pending_cb`, `pending_ctx`

Hunk cross-owner cần Bách review ở Task 3 — tên Sơn mà `server/game/*` gọi: `rating_apply`, `store_match_start`, `store_events`, `store_finalize`, `store_abort`, `store_account_get_by_id`, `store_profile_stats`, `store_get_db`. `RoomStart` (`rooms.h`) là kiểu `matchesStart` nhận; giữ nguyên layout khi đổi tên field.

## Cần Bách xem

- Framing và `Conn` (`shared/`), command/push trong `server/app.c`, seam native ↔ JS (`desktop/bridge.c`, `net_session.c`).
- `desktop/main.c`: các callback dựng UI (`on_decide_policy`, `on_load_changed`, `on_window_destroy`) giữ tên và thân hàm; chỉ dấu `{` và tham chiếu biến global đổi.
