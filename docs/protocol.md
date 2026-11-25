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
| RESUME | token. Trả `me`, không trả lại token. Chỉ native desktop gửi |
| REGISTER | email, password, name, avatar. Trả `userId` |
| LOGIN | email, password. Trả token, me |
| PROFILE_GET / PROFILE_UPDATE | userId tùy chọn / name, avatar (userId nếu có phải là chính mình) |
| LOBBY_GET, FRIEND_LIST, LEADERBOARD_GET | Không có |
| FRIEND_REQUEST / FRIEND_REPLY | tag / requestId, accept |
| ROOM_CREATE, ROOM_MODE | mode |
| ROOM_JOIN / ROOM_BOT / ROOM_KICK | code / seat, level / seat |
| ROOM_LEAVE, ROOM_START, PARTY_LEAVE, QUEUE_LEAVE | Không có |
| INVITE_SEND / INVITE_REPLY | userId, kind (`room` hoặc `party`), mode (bắt buộc khi `party`: `2v2` hoặc `side`) / inviteId, accept |
| QUEUE_JOIN | mode (`classic`, `2v2`, `side` hoặc `draft`) |
| PLAY | matchId, turnNo, cardId, color (bắt buộc với Wild) |
| DRAW, PASS | matchId, turnNo |
| SAY_UNO, MATCH_LEAVE | matchId |
| DRAFT_PICK / CHAT | matchId, draftTurn, index / matchId, chatId |

Response dùng `OK` với object kết quả, hoặc `ERROR` với `{"code":"…","message":"…"}` (luôn đủ hai trường). Server push dùng FRIEND_STATE, INVITE, PARTY_STATE, QUEUE_STATE, ROOM_STATE, GAME_STATE, GAME_CHAT, MATCH_RESULT, PROFILE_STATE hoặc SERVER_SHUTDOWN. LOBBY_STATE được dành sẵn nhưng server hiện chưa phát; lobby lấy bằng LOBBY_GET và FRIEND_STATE. Client so sánh revision theo từng entity để bỏ snapshot cũ.

Mã lỗi gồm BAD_REQUEST, UNAUTHENTICATED, FORBIDDEN, BUSY, NOT_FOUND, ROOM_FULL, INVALID_ACTION, NOT_YOUR_TURN, STALE_TURN, STALE_REQUEST, RATE_LIMITED, STORAGE_ERROR và SERVER_FULL.

Bên nhận đóng kết nối khi framing/envelope hỏng. Tầng xử lý command kiểm tra opcode, chiều bản tin, payload và quyền theo session. Request ID phải tăng nghiêm ngặt trên từng kết nối. Khi ID cũ hoặc trùng, server trả STALE_REQUEST và bỏ qua thao tác. Sau reconnect, client lấy snapshot mới. UserId, roomId, matchId và tag trên wire là chuỗi opaque. Seat là số nguyên tuyệt đối 0..3. Server lấy actor từ session.

Rời phiên (đóng socket, hết heartbeat, LOGOUT) xử lý giống nhau: ngoài trận, người đó rời ngay queue (cả entry party), invite, party (party giải tán) và phòng đang chờ; trong trận, ghế chuyển offline nhưng vẫn giữ cho RESUME hoặc LOGIN lấy lại, và trận bị hủy (aborted) ngay khi không còn human nào kết nối. Với LOGOUT, socket vẫn mở và nhận các push đóng (QUEUE_STATE `active:false`, PARTY_STATE giải tán) trước OK; session bị xóa nên token hết hiệu lực. Mỗi socket chỉ mang một tài khoản: LOGIN hoặc RESUME thành công cho tài khoản khác trên socket đã đăng nhập sẽ đăng xuất tài khoản cũ như LOGOUT trước khi gắn tài khoản mới. Token sai hoặc hết hạn không làm mất tài khoản đang gắn.

Server đóng kết nối không có frame hợp lệ nào trong cửa sổ heartbeat (`--heartbeat-ms`, mặc định 15.000 ms); socket mới cũng có đúng một cửa sổ để gửi frame đầu. Desktop gửi PING mỗi 5 giây và coi kết nối chết sau 15 giây không nhận được gì. Khi hết chỗ, server gửi `ERROR {"code":"SERVER_FULL",…}` với id 0 rồi đóng socket. Khi tắt, server push `SERVER_SHUTDOWN {}` tới mọi session đã đăng nhập trước khi đóng.

## Payload trạng thái

Mọi id người dùng là chuỗi thập phân. `me` của LOGIN và RESUME là `{userId, tag, name, avatar, elo}`, với tag dạng `UNO#<userId>`. Sau LOGIN OK hoặc RESUME OK, server gửi lại cho socket mới GAME_STATE (nếu đang trong trận), ROOM_STATE, PARTY_STATE, QUEUE_STATE (nếu đang ở đó) và FRIEND_STATE. Server chỉ gửi lại những gì còn tồn tại, không có snapshot "không có" nào: khi server đã thấy kết nối rớt (EOF/reset hoặc hết 15 giây heartbeat), party đã giải tán, ghế ở phòng đang chờ và entry queue đã bị xóa, còn trận có thể đã kết thúc hoặc bị hủy trong lúc offline. Vì vậy client không được giữ room/party/queue qua lần mất kết nối; xem hợp đồng `reconnecting` ở phần Desktop bridge.

| Bản tin | Payload |
|---|---|
| ROOM_STATE | `{revision, roomId, code, mode, ranked, hostSeat, seats}`. `seats` có 4 phần tử: `null`, bot `{seat, isBot:true, name, avatar, botLevel}` hoặc người `{seat, isBot:false, userId, name, avatar}` |
| PARTY_STATE | `{partyId, leaderId, revision, mode, members:[userId]}`; `mode` là `2v2` hoặc `side`. Khi party giải tán: `leaderId:"0"`, `members:[]`, revision tăng |
| INVITE | `{inviteId, senderId, senderName, kind}`, thêm `roomId` khi `kind` là `room`, thêm `mode` khi `kind` là `party`. INVITE_SEND trả `{inviteId}` |
| QUEUE_STATE | `{mode, active, players:[{userId, name, avatar}], revision, matchId?}`. Người rời queue nhận `active:false` không có `players`; người được ghép trận nhận `active:false` kèm `matchId`. QUEUE_JOIN trả `{queue: <QUEUE_STATE>}`, QUEUE_LEAVE trả `{}` |
| FRIEND_STATE | `{friends:[{userId, tag, name, avatar, elo, presence, online, pending, incoming, requestId?}]}`, cũng là kết quả FRIEND_LIST. `presence` là `offline`, `idle`, `online` hoặc `in_match`; chỉ bạn đã chấp nhận mới thấy presence thật. `requestId` (bằng userId người gửi) chỉ có ở lời mời đến |
| PROFILE_STATE | `<profile>`, push cho chính chủ sau PROFILE_UPDATE. PROFILE_GET và PROFILE_UPDATE trả `{profile}` |
| LEADERBOARD_GET | Trả `{entries:[{userId, tag, name, avatar, elo, rank}]}`, tối đa 20, ELO giảm dần |
| LOBBY_GET | Trả `{friends, leaderboard}` cùng dạng FRIEND_STATE và `entries` |
| MATCH_RESULT | `{matchId, mode, ranked, winnerSeat, winnerTeam, durationMs, players}`; mỗi player có `eloBefore` và `eloAfter` |

`<profile>` là `{userId, tag, name, avatar, elo, rank, stats, badges, modes, history}`. `rank` là `bronze`, `silver`, `gold` hoặc `diamond`. `stats` gồm `matches, wins, teamWins, plus4, unoCalls, streak, bestStreak, peakElo`. `badges` là mảng key (`first-win`, `win-100`, `streak-10`, `plus4`, `uno-master`, `team`, `matches-500`, `diamond`). `modes` là `[{key, matches, wins}]` cho 4 mode. `history` là tối đa 20 trận mới nhất `{matchId, mode, won, delta, when}`, `when` tính bằng epoch ms. Profile chỉ chứa thông tin công khai, nên ai đăng nhập cũng xem được.

FRIEND_REQUEST nhận `tag` của người kia và trả `{}`; NOT_FOUND khi không có tài khoản, INVALID_ACTION khi hai bên đã liên kết. FRIEND_REPLY nhận `requestId` của lời mời đến. Sau mỗi thay đổi, cả hai bên nhận FRIEND_STATE đầy đủ (client thay cả danh sách). PROFILE_UPDATE còn phát FRIEND_STATE cho bạn bè và ROOM_STATE nếu phòng đang chờ.

ROOM_KICK gửi cho người bị kick cùng ROOM_STATE như các ghế còn lại; ghế của họ không còn trong `seats`, nên client coi như đã rời phòng. Khi trận của phòng kết thúc, phòng được giải phóng sau MATCH_RESULT mà không có ROOM_STATE nào nữa. QUEUE_JOIN trả BUSY khi một thành viên đang ở phòng, trận hoặc queue; INVALID_ACTION khi `mode` khác mode của party; SERVER_FULL khi queue của mode đã đầy.

## Desktop bridge

`CONNECTION_STATE` là event cục bộ do native phát cho JS, không bao giờ đi trên TCP: `{status, me?, sessionExpired?}` với `status` là `connecting`, `connected` hoặc `reconnecting`. Mất kết nối chuyển sang `reconnecting` và native tự kết nối lại với backoff 1 đến 8 giây. Nếu đang giữ token, native gửi RESUME và chỉ báo `connected` khi có kết quả: thành công kèm `me`, thất bại kèm `sessionExpired:true` (JS phải đăng nhập lại). Khi nhận `reconnecting`, JS phải bỏ room, party, queue và cả game (trận có thể đã kết thúc khi offline), rồi dựng lại chỉ từ các snapshot tới sau RESUME OK; thành viên đã mất trong lúc offline không được báo riêng. Request JS gửi khi chưa `connected` bị trả ngay `ERROR {"code":"DISCONNECTED","message":"…"}` và không được gửi lại sau. RESUME không có trong whitelist của JS; native giữ token và xóa trường `token` khỏi mọi payload chuyển cho JS.

## Ownership của API C

Khởi tạo một `FrameReader` bằng `{0}` hoặc `frame_reader_reset` cho mỗi byte stream. Gọi `frame_feed` với từng phần dữ liệu nhận được. Reader giữ phần frame còn thiếu. Callback chạy đồng bộ một lần cho mỗi envelope hoàn chỉnh. Con trỏ envelope, type và payload được mượn đến khi callback trả về. Để giữ payload lâu hơn, gọi `json_object_get` và giải phóng bằng `json_object_put`; sao chép type nếu cần.

Callback trả `OK` để tiếp tục đọc. Status khác dừng lần feed ngay và được trả nguyên cho caller. API không trả offset để tiếp tục phần còn lại của chunk sau lỗi. Để tiếp tục kết nối khi command bị từ chối, callback xếp response ERROR rồi trả OK. Khi đóng hoặc thay stream, reset reader để bỏ frame dở.

`frame_encode` mượn type và payload trong lần gọi. Caller sở hữu output buffer. Thành công ghi cả header và JSON vào buffer, trả số byte qua `written`. Khi lỗi, `written` bằng 0.

[Fixture request, response và push](../tests/fixtures/requests.json) · [Schema nghiệp vụ, giới hạn và snapshot](superpowers/specs/2026-11-11-ubuntu-tcp-design.md)
