# UNOP/1 trên TCP

Mỗi bản tin bắt đầu bằng độ dài JSON (4 byte unsigned big-endian), theo sau là đúng số byte UTF-8 đó. Độ dài hợp lệ từ 1 đến 65.536, không tính header. Một lần đọc TCP có thể chỉ chứa một phần header/body hoặc chứa nhiều frame liên tiếp. Ví dụ, body HELLO dưới đây dài 42 byte nếu viết liền như trong fixture (hãy dùng độ dài chuỗi đã serialize khi gửi).

```json
{"v":1,"id":1,"type":"HELLO","payload":{}}
```

Body là một object JSON, độ sâu tối đa 16 tính cả object ngoài cùng. Byte cuối phải kết thúc object. Byte NUL, UTF-8 hỏng, dữ liệu thừa hoặc trường bắt buộc sai kiểu làm hỏng frame. Với key trùng, json-c lấy giá trị cuối.

| Trường | Kiểu và ý nghĩa |
|---|---|
| `v` | Integer bằng 1 |
| `id` | Integer 1..2147483647 cho request và response tương ứng. Push dùng 0 |
| `type` | Chuỗi opcode khác rỗng |
| `payload` | Object, dùng `{}` khi không có tham số |

## Command và phản hồi

| Request | Trường payload |
|---|---|
| HELLO, PING, LOGOUT | Không có. HELLO trả `serverTimeMs` |
| RESUME | token |
| REGISTER | email, password, name, avatar |
| LOGIN | email, password. Trả token, me |
| PROFILE_GET / PROFILE_UPDATE | userId tùy chọn / name, avatar |
| LOBBY_GET, FRIEND_LIST, LEADERBOARD_GET | Không có |
| FRIEND_REQUEST / FRIEND_REPLY | tag / requestId, accept |
| ROOM_CREATE, ROOM_MODE | mode |
| ROOM_JOIN / ROOM_BOT / ROOM_KICK | code / seat, level / seat |
| ROOM_LEAVE, ROOM_START, PARTY_LEAVE, QUEUE_LEAVE | Không có |
| INVITE_SEND / INVITE_REPLY | userId, kind (`room` hoặc `party`) / inviteId, accept |
| QUEUE_JOIN | mode |
| PLAY | matchId, turnNo, cardId, color (bắt buộc với Wild) |
| DRAW, PASS | matchId, turnNo |
| SAY_UNO, MATCH_LEAVE | matchId |
| DRAFT_PICK / CHAT | matchId, draftTurn, index / matchId, chatId |

Response dùng `OK` với object kết quả, hoặc `ERROR` với `{"code":"…","message":"…"}`. Server push dùng LOBBY_STATE, FRIEND_STATE, INVITE, PARTY_STATE, QUEUE_STATE, ROOM_STATE, GAME_STATE, MATCH_RESULT, PROFILE_STATE hoặc SERVER_SHUTDOWN. Client so sánh revision theo từng entity để bỏ snapshot cũ.

Mã lỗi gồm BAD_REQUEST, UNAUTHENTICATED, FORBIDDEN, BUSY, NOT_FOUND, ROOM_FULL, INVALID_ACTION, NOT_YOUR_TURN, STALE_TURN, STALE_REQUEST, RATE_LIMITED, STORAGE_ERROR và SERVER_FULL.

Bên nhận đóng kết nối khi framing/envelope hỏng. Tầng xử lý command kiểm tra opcode, chiều bản tin, payload và quyền theo session. Request ID phải tăng nghiêm ngặt trên từng kết nối. Khi ID cũ hoặc trùng, server trả STALE_REQUEST và bỏ qua thao tác. Sau reconnect, client lấy snapshot mới. UserId, roomId, matchId và tag trên wire là chuỗi opaque. Seat là số nguyên tuyệt đối 0..3. Server lấy actor từ session.

## Ownership của API C

Khởi tạo một `FrameReader` bằng `{0}` hoặc `frame_reader_reset` cho mỗi byte stream. Gọi `frame_feed` với từng phần dữ liệu nhận được. Reader giữ phần frame còn thiếu. Callback chạy đồng bộ một lần cho mỗi envelope hoàn chỉnh. Con trỏ envelope, type và payload được mượn đến khi callback trả về. Để giữ payload lâu hơn, gọi `json_object_get` và giải phóng bằng `json_object_put`; sao chép type nếu cần.

Callback trả `OK` để tiếp tục đọc. Status khác dừng lần feed ngay và được trả nguyên cho caller. API không trả offset để tiếp tục phần còn lại của chunk sau lỗi. Để tiếp tục kết nối khi command bị từ chối, callback xếp response ERROR rồi trả OK. Khi đóng hoặc thay stream, reset reader để bỏ frame dở.

`frame_encode` mượn type và payload trong lần gọi. Caller sở hữu output buffer. Thành công ghi cả header và JSON vào buffer, trả số byte qua `written`. Khi lỗi, `written` bằng 0.

[Fixture request, response và push](../tests/fixtures/requests.json) · [Schema nghiệp vụ, giới hạn và snapshot](superpowers/specs/2026-11-11-ubuntu-tcp-design.md)
