# UNO Ubuntu TCP — thiết kế để duyệt

## 1. Phạm vi và trạng thái

Yêu cầu đã thống nhất: chỉ chạy Ubuntu; server và client mạng viết bằng C dùng POSIX TCP socket; không WebSocket; giữ React làm giao diện desktop; hai máy demo qua LAN. Hoàn thành toàn bộ bảng chức năng Bách–Sơn, không chỉ demo đánh bài.

Đây là bản thiết kế để duyệt cùng plan, chưa phải backend đã triển khai. Chỉ tạo tài liệu trong lượt này, **không commit, không tạo/chuyển/xóa branch, không triển khai code**. Hai branch local tạo ở lượt trước vẫn tồn tại; không tự ý xóa. Các quyết định dưới đây là đề xuất kỹ thuật của người lập plan, chưa phải những chi tiết người dùng đã duyệt riêng.

Hiện trạng kiểm tra trên `main`, commit `0cd100b`:
- Chỉ có `client/`: React 18, Vite 5, Vitest 2; không có mã C/backend.
- `client/src/game/engine.js`, `draft.js`, `rules.js` là tham chiếu luật local, không phải nguồn sự thật khi online.
- `useUnoGame.js` chạy bot/timer ở client; `GameTable.jsx` tự tính kết quả/ELO.
- Login, phòng, matchmaking, bạn bè, hồ sơ và bảng xếp hạng dùng dữ liệu giả/lưu local.
- `ARCHITECTURE.md` chưa được Git theo dõi và chứa hướng WebSocket cũ. Tài liệu mới này là hướng đề xuất thay thế; chưa sửa tài liệu cũ khi lập plan.

## 2. Nền tảng và đường truyền

- Môi trường chuẩn đề xuất: **Ubuntu 24.04 LTS amd64**, desktop GTK 3 + WebKitGTK API 4.1; C11, GCC, GNU Make, pkg-config, Python 3 cho integration test, Node 22 cho build React.
- React build thành file tĩnh, Vite `base: './'`, dùng HashRouter. Native host đăng ký scheme `uno://app/` để đọc `client/dist/`, MIME đúng và chặn path traversal; không cần HTTP server nội bộ.
- Cầu nối duy nhất: JS `window.webkit.messageHandlers.uno.postMessage(JSON.stringify(request))`; native trả sự kiện `CustomEvent('uno:message', {detail: envelope})` bằng chuỗi serialize JSON an toàn, không nối dữ liệu thô vào mã JS.
- Native host chỉ load origin `uno://app`, chặn điều hướng/popup ngoài và iframe gửi bridge. Chỉ nhận các command trong whitelist, không cấp API đọc file/chạy lệnh/đổi host cho JS. Host/port cấu hình ở CLI native.
- Client C dùng non-blocking `connect/send/recv` và GLib fd sources trên GTK loop, không chặn UI, không dùng mạng của JavaScript. Server C dùng `poll()` non-blocking, một luồng sở hữu toàn bộ room/game/session. Không thêm epoll, timer heap hoặc worker DB trước khi có nhu cầu đo được.
- Hash mật khẩu chạy trên **một worker pthread có queue giới hạn 8 job**; kết quả trả qua pipe non-blocking về poll loop. Worker không giữ con trỏ session; dùng connection ID + generation để loại kết quả của kết nối đã đóng/tái sử dụng. SQLite chỉ được truy cập trên main loop.
- Default `0.0.0.0:9090` ở server; client bắt buộc `--host IP --port 9090`. Tối đa 64 kết nối, 16 phòng; output queue mỗi connection tối đa 256 KiB; đóng slow consumer khi vượt giới hạn, không chặn cả hệ thống.
- Không TLS trong phạm vi demo LAN này: mật khẩu/token đi qua TCP không mã hóa. Chỉ dùng tài khoản demo, mạng tin cậy; triển khai Internet cần thêm TLS. Hash trong DB không bảo vệ đường truyền.

## 3. Giao thức UNOP/1 đề xuất

Mỗi frame: **4 byte unsigned big-endian length + JSON UTF-8** (length chỉ tính JSON), 1..65536 byte. Dùng json-c để parse/serialize; không tự viết JSON parser hoặc gửi raw C struct. Buffer mỗi connection giữ frame chưa đủ; xử lý được nhiều frame trong một recv. Parser yêu cầu đúng một JSON object, không trailing garbage/NUL, sâu tối đa 16, trường bắt buộc đúng type/range. Key trùng không chặn riêng: json-c giữ giá trị cuối và server là bên parse duy nhất. Frame lỗi đóng connection; command hợp lệ về cấu trúc nhưng sai nghiệp vụ trả ERROR.

Request `{v:1,id:1,type:"PLAY",payload:{matchId:"...",turnNo:4,cardId:"c17",color:"red"}}`.
Response `{v:1,id:1,type:"OK",payload:{...}}` hoặc `{v:1,id:1,type:"ERROR",payload:{code:"NOT_YOUR_TURN",message:"..."}}`.
Push `{v:1,id:0,type:"GAME_STATE",payload:{...}}`. `id` request là integer 1..2147483647 tăng nghiêm ngặt trên connection; `id <= lastId` trả `STALE_REQUEST`, không thực hiện lại. Response giữ nguyên id. Client không retry tự động command gây thay đổi; reconnect lấy snapshot mới.

Các command và payload:

| Nhóm | type và payload |
|---|---|
| Kết nối | HELLO `{}` → OK `{serverTimeMs}`; PING `{}` → OK; RESUME `{token}`; LOGOUT `{}` |
| Tài khoản | REGISTER `{email,password,name,avatar}`; LOGIN `{email,password}` → OK `{token,me}`; PROFILE_GET `{userId?}`; PROFILE_UPDATE `{name,avatar}` |
| Sảnh | LOBBY_GET `{}`; FRIEND_REQUEST `{tag}`; FRIEND_REPLY `{requestId,accept}`; FRIEND_LIST `{}`; LEADERBOARD_GET `{}` |
| Phòng | ROOM_CREATE `{mode}`; ROOM_JOIN `{code}`; ROOM_LEAVE `{}`; ROOM_BOT `{seat,level}`; ROOM_KICK `{seat}`; ROOM_MODE `{mode}`; ROOM_START `{}` |
| Mời | INVITE_SEND `{userId,kind}` với kind `room` hoặc `party`; INVITE_REPLY `{inviteId,accept}`; PARTY_LEAVE `{}` |
| Ghép trận | QUEUE_JOIN `{mode}`; QUEUE_LEAVE `{}`; party được lấy từ session, không tin danh sách user client gửi |
| Trận | PLAY `{matchId,turnNo,cardId,color?}`; DRAW/PASS `{matchId,turnNo}`; SAY_UNO `{matchId}`; DRAFT_PICK `{matchId,draftTurn,index}`; MATCH_LEAVE `{matchId}`; CHAT `{matchId,chatId}` |

Push: `LOBBY_STATE`, `FRIEND_STATE`, `INVITE`, `PARTY_STATE`, `QUEUE_STATE`, `ROOM_STATE`, `GAME_STATE`, `MATCH_RESULT`, `PROFILE_STATE`, `SERVER_SHUTDOWN`. Snapshot luôn có revision tăng, client bỏ revision cũ theo từng entity. Lỗi chuẩn: BAD_REQUEST, UNAUTHENTICATED, FORBIDDEN, BUSY, NOT_FOUND, ROOM_FULL, INVALID_ACTION, NOT_YOUR_TURN, STALE_TURN, STALE_REQUEST, RATE_LIMITED, STORAGE_ERROR, SERVER_FULL.

Server suy ra user/seat từ session; client không được gửi actor/seat của mình. `roomId`, `matchId`, `userId`, `tag` là string opaque; seat chỉ là integer 0..3 tuyệt đối do server quản lý. Avatar chỉ là ID thuộc danh sách `client/src/data/avatars.jsx`; không nhận URL/path. Email ASCII lowercase 3..254 byte, password 6..128 byte, name UTF-8 1..16 Unicode code point (tối đa 64 byte); không NUL/control; tag `UNO#` + user ID thập phân bất biến. CHAT chỉ dùng ID của 8 câu/emoji QUICK_CHAT hiện có. ID đếm SQLite không vượt integer JS an toàn; truyền wire dưới dạng string.

Giới hạn request 30/s/connection (burst 60), login/register mặc định tối đa 5/phút/IP (chỉnh bằng cờ CLI server `--auth-limit N` cho integration test chạy cùng `127.0.0.1`); giới hạn trước khi đẩy worker hash. Session token 32 byte random libsodium, wire 64 hex; giữ trong bộ nhớ native, không localStorage/log. Một phiên active/user; login mới thay connection cũ, vẫn giữ ghế nếu còn trận. Hết phiên sau 24h hoặc LOGOUT; reconnect lấy lại ghế khi trận còn chạy.

## 4. Luật, snapshot và thời gian

- Bộ bài **112 lá** như deck.js: 108 chuẩn + 1 Discard All/màu. ID `c1`..`c112` ổn định trong một trận. Shuffle Fisher–Yates; production seed từ libsodium, test seed cố định; không lộ seed/thứ tự deck.
- Classic: 7 lá/người, 4 ghế, ghế 0 bắt đầu; lá mở đầu là số. Ai hết bài trước thắng, ba người còn lại cùng thua (không tự xếp hạng 2–4).
- 2v2: A={0,2}, B={1,3}; side: A={0,1}, B={2,3}; một người hết bài thì đội thắng. Không suy ra đội lại từ ghế UI sau khi xoay góc nhìn.
- Skip bỏ một lượt; Reverse đảo hướng; +2/+4 rút ngay và mất lượt, không cộng dồn; Wild +4 được đánh như luật local hiện tại, không challenge; Wild phải có màu hợp lệ trong cùng PLAY. Chọn/hủy màu chỉ là trạng thái UI, không có pendingWild khóa server.
- Discard All bỏ toàn bộ cùng màu, lá phụ không kích hoạt hiệu ứng. Lá cuối thắng ngay, theo engine local hiện tại; không áp hiệu ứng lá cuối sau khi đã kết thúc.
- DRAW rút 1; nếu đánh được, chỉ được đánh chính lá mới rút hoặc PASS; nếu không đánh được thì chuyển lượt. Deck hết: giữ top discard, shuffle phần còn lại; nếu không thể rút thêm thì chuyển lượt, không truy cập ngoài buffer. Không đủ bài để rút phạt thì rút số còn có thể.
- Draft: 4 lá đầu/người + 4 vòng; mỗi vòng có 4 lá chung, mỗi người lấy 1; người bắt đầu vòng r là r, thứ tự `(r + pickWithinRound) % 4`; kết thúc 16 pick thì 8 lá/người. Mỗi pick 20s, timeout tự lấy slot thấp nhất còn bài, không phạt rút.
- Lượt thường 20s, CLOCK_MONOTONIC. Timeout rút 1 nếu chưa DRAW, rồi chuyển lượt; DRAW không reset deadline. Các callback cũ bị loại theo turnNo/match generation.
- **[Chờ Bách xác nhận]** Ân hạn UNO 3s dưới đây ngược quyết định 25/10 (phạt UNO ngay). Chốt trước Task 9.
- UNO có thể hô trước khi đánh lúc đang có 2 lá hoặc trong 3s sau khi xuống 1 lá; hết hạn +2, đúng một lần. Watch độc lập mỗi ghế; người khác đánh không xóa watch. Khi chủ watch rút thêm bài hoặc trận kết thúc thì hủy watch. Không sao chép lỗi unoWatch đơn của JS sang C.
- Tick server trước khi xử lý input cùng vòng loop; `now >= deadline` thì timeout thắng cuộc đua. Client chỉ hiển thị countdown từ thời gian server, không tự gửi TIMEOUT/UNO_PENALTY.
- Bot `easy`: random trong nước hợp lệ; `normal`: heuristic như bot.js; `hard`: ưu tiên giảm số lá/không phạt đồng đội và chặn đối thủ còn ít bài, chỉ dùng view được phép thấy. Không hứa hard luôn thắng normal. Bot đi sau 700ms, tự draft và hô UNO; mọi bot action đi qua cùng game_apply.
- `GAME_STATE`: matchId/revision, viewerSeat, mode, teams, turn, turnNo, direction, currentColor, topCard, drawCount, phase, deadline/serverNowMs/durationMs, draftTurn/offers, winner, legalCardIds/canDraw/canPass/canSayUno, players[4]. durationMs tính từ khi phase PLAYING bắt đầu; client chỉ nội suy hiển thị từ snapshot mới nhất. Player có seat/userId/name/avatar/team/cardCount/connected/isBot/botLevel; `hand` chỉ có cho chính mình và đồng đội. Không có drawPile, seed, hidden IDs hay log nội bộ trong snapshot.
- View xoay người xem xuống dưới trong React; command vẫn dùng ID và turn tuyệt đối. Bài úp UI tạo placeholder từ cardCount, không giả dữ liệu bài thật trong wire.

## 5. Sảnh, bạn bè, phòng và phiên

- Presence suy ra từ connection và activity: offline / online (đang sảnh/phòng/hàng chờ) / idle (sảnh sẵn sàng) / in_match. Không nhận status client tự khai. Push tới người đang ở sảnh và bạn bè; không phát email/token.
- Kết bạn bằng tag, có request accept/reject, không tự kết bạn hoặc request trùng/ngược; lưu DB. Mời người offline/busy không hợp lệ; invite hết hạn 30s; accept chỉ có hiệu lực nếu phòng còn chỗ và người nhận vẫn rảnh.
- Room 4 ghế human/bot/empty, 1 host; mã 6 ký tự uppercase A-Z/0-9 sinh random, retry collision; mã là mã vào phòng chứ không lưu mật khẩu phụ. Host được đổi mode, thêm bot chọn level, kick; chỉ start khi 4 ghế đầy và không ai đang ở hoạt động khác. Host rời trước trận thì chuyển cho human còn lại có seat nhỏ nhất; không còn human thì hủy phòng.
- Party tối đa 2 human ở mode team; matchmaking giữ hai người cùng đội. Queue FIFO theo mode, không ghép khác mode, không auto thêm bot vào ranked, không duplicate user giữa room/party queue/match. Party 2 người được ghép với party 2 người hoặc 2 solo; hủy queue/rớt mạng thì bỏ cả entry party. Room riêng unranked; chỉ matchmaking 4 human mới ranked.
- PING client mỗi 5s, server timeout không có frame hợp lệ sau 15s; socket EOF/reset xử lý ngay. **[Bách chốt 11/11]** Trong trận, disconnect không đổi gì về luật: không có bot đánh thay, không có timer offline. Ghế vẫn thuộc account đó; timer lượt 20s chạy tiếp như người đang online mà không thao tác, hết giờ xử lý đúng theo luật timeout §4. RESUME cùng account lấy lại ghế bất kỳ lúc nào khi trận còn chạy, trả đúng view hiện tại. Chỉ MATCH_LEAVE (chủ động rời) mới chuyển ghế cho bot đến hết trận, account không reclaim lại; user không được vào trận khác trước khi trận cũ chốt kết quả. Ngoài trận dọn queue/invite/room membership ngay.
- Nếu không còn human nào đang kết nối trong trận thì kết thúc aborted ngay (không chạy trận rỗng bằng timeout), ghi log, không cộng ELO/thành tựu. Server restart không khôi phục trận đang chạy; các trận active còn lưu DB được đánh dấu aborted khi startup.

## 6. Lưu trữ và phân công

SQLite là nguồn lưu duy nhất: accounts (email unique, password hash, name/avatar/tag/ELO), friends/requests, matches (UUID, status, mode/ranked/result), match_players (ghế và thống kê), match_events (seq/type/payload), achievements/progress. Schema do Sơn sở hữu, Bách dùng API repository; không có DB riêng cho hồ sơ/log.

Password dùng libsodium `crypto_pwhash_str` + `crypto_pwhash_str_verify`, INTERACTIVE limits; không tự hash hoặc tự quản salt. Prepared statements cho toàn bộ input. Match start phải persist thành công trước khi chơi. Sau mỗi action hợp lệ, tính candidate state + event batch, lưu transaction rồi mới publish candidate/snapshot; lỗi đĩa giữ state cũ, trả STORAGE_ERROR, tạm dừng trận (timer không lặp phạt) đến khi lưu được hoặc admin dừng. Token/password không vào event log. Kết quả, ELO, tiến độ thành tựu và status finalized cập nhật trong **một transaction**, unique matchId chống ghi lặp; chỉ push MATCH_RESULT khi commit thành công. Xuất JSON lịch sử theo matchId từ DB sau trận, không có nguồn log thứ hai phải đồng bộ.

ELO ban đầu 1500, K=32. Solo: mỗi người so với average ELO ba người còn lại, score=1 nếu thắng, 0 nếu thua; team: average đội so với average đối thủ, cùng delta cho hai thành viên. `round` half away from zero (C lround), clamp ELO >=0. Không yêu cầu zero-sum. Unranked/aborted không đổi. Hạng chỉ 4 bậc theo yêu cầu: Đồng <1200, Bạc 1200..1399, Vàng 1400..1799, Kim cương >=1800; bỏ Bạch kim/Cao thủ giả hiện có. Top 20 theo ELO desc rồi userId asc.

Thành tựu lấy từ sự kiện server và kết quả finalized: first-win=1, win-100=100, streak-10=10, plus4=50, uno-master=300, team=50, matches-500=500, diamond=1800 ELO cao nhất. Giữ tên/icon hiện có; chỉnh mô tả diamond từ 2200 thành 1800 để khớp hạng. Không hiển thị level/xp/title giả nếu chưa có nghiệp vụ, không thêm hệ thống XP.

| Chủ sở hữu | File/phạm vi |
|---|---|
| Sơn | shared/protocol.*, shared/net.*, server/{main,app,accounts,auth_worker,store,social,rooms,queue}.*, server/schema.sql, desktop/*, build chung |
| Bách | server/{game,bot,match,view,profile}.*, client/src/** (toàn bộ UI tích hợp), client/vite.config.js |
| Chốt chung trước khi chia | shared/contracts.h, docs/protocol.md, fixtures JSON; Task 1 do Bách code và Sơn review, sau khi duyệt Sơn là người sửa các file này (cùng shared/protocol.*, Makefile) để tránh xung đột. contracts.h chỉ chứa kiểu cả hai cùng dùng; Card/Intent/Game/GameView nằm ở server/game.h, view.h của Bách |

Nhánh chỉ là cách chia quyền sở hữu sau khi được phép; chưa thực hiện Git mutation theo plan. Hai người triển khai được song song khi Task 1 chốt contract; phải tích hợp sớm bằng client C headless trước khi hoàn thiện GUI.

## 7. Nguồn kỹ thuật đã kiểm tra

- [Ubuntu 24.04: WebKitGTK API 4.1 development package](https://packages.ubuntu.com/noble/libwebkit2gtk-4.1-dev).
- [WebKitGTK: JS → native handler](https://webkitgtk.org/reference/webkit2gtk/2.41.90/method.UserContentManager.register_script_message_handler.html), [native → JS evaluation](https://webkitgtk.org/reference/webkit2gtk/stable/method.WebView.evaluate_javascript.html).
- [json-c: parse/serialize JSON trong C](https://json-c.github.io/json-c/json-c-current-release/doc/html/index.html).
- [libsodium: password hash/verify](https://doc.libsodium.org/password_hashing/default_phf).
- [SQLite transaction](https://www.sqlite.org/lang_transaction.html).

Các API/package được tra tài liệu; chưa cài hoặc chạy trên Ubuntu. Đây là điểm cần kiểm chứng đầu tiên khi thực thi.
