# Kế hoạch refactor mã C và cấu trúc thư mục UNO

**Mục tiêu** — Chuẩn hóa mã C theo [QUY_UOC_CONVENTION.md](../../../QUY_UOC_CONVENTION.md), sau đó tổ chức lại source và test theo module. Phân việc cho Sơn và Bách theo phần mỗi người đã phụ trách.

**Mốc khảo sát** — `main` tại `f9abc16`, gồm 55 file `.c`/`.h` được Git theo dõi. Working tree đang có thay đổi ở `server/match.c`, `server/profile.{c,h}`, `tests/test_match.c` và `tests/test_profile.c`. Hai bài tích hợp `test_full_flow.py`, `test_resilience.py` của Task 15 đang là file chưa được Git theo dõi.

## Phạm vi

- Mã C và header trong `server/`, `shared/`, `desktop/` và `tests/`. Với desktop, chuẩn hóa phần kết nối TCP, phiên native và bridge. Phần dựng giao diện GTK/WebKit nằm ngoài phạm vi chỉnh sửa, trừ include hoặc tham chiếu đến tên C đã đổi.
- `Makefile`, include và test là các nơi cần cập nhật theo tên hoặc path mới. `schema.sql` đi cùng Store như tài liệu schema. `client/` nằm ngoài phạm vi refactor.
- Giữ hành vi hiện tại của luật UNO, bot, phòng, party, queue, ELO, hồ sơ và reconnect. Giữ giao thức UNOP/1, opcode, JSON key, mã lỗi, schema và thứ tự giao dịch SQLite, CLI, tên binary, path asset và sự kiện bridge.
- Môi trường kiểm chứng chính là Ubuntu 24.04, C11 với `-Wall -Wextra -Wpedantic -Werror`. Giữ build server/test headless độc lập với GTK. Commit của đợt refactor theo quy ước của nhóm, không có trailer `Co-Authored-By`.

## Phân công theo plan cũ

| Phần | Người sửa chính | Căn cứ và người review |
|---|---|---|
| `shared/contracts.h`, `shared/protocol.{c,h}`, `tests/test_protocol.c`, `Makefile` | **Sơn** | Bách viết Task 1 ban đầu. Thiết kế và plan tổng giao phần dùng chung cho Sơn sau khi duyệt. Bách review thay đổi framing, fixtures và interface chung. |
| `shared/net.{c,h}`, `server/main.c`, `server/app.{c,h}`, `desktop/tcp_client.{c,h}`, `tests/test_net.c` | **Sơn** | Task 2 và phần đấu nối dispatcher của Task 9. Bách review chỗ dispatcher gọi Matches. |
| `server/{accounts,auth_worker,rooms,queue,rating,social,store}.{c,h}`, `server/schema.sql`, `tests/test_{accounts,rooms,queue,rating,social,store}.c` | **Sơn** | Task 3–5, Task 10 và phần social/Store của Task 11. Sơn giữ `PROFILE_GET`/`PROFILE_UPDATE` và SQL. Bách review seam Store ↔ Match/Profile. |
| `desktop/{main.c,bridge.c,bridge.h,net_session.c,net_session.h}`, `tests/test_{bridge,net_session}.c` | **Sơn** | Task 12, bao gồm TCP heartbeat, RESUME và bridge C. Bách dùng bản build hiện có để kiểm tra luồng giao diện. |
| `server/{game,bot,view,match,profile}.{c,h}`, `tests/test_{game,modes,bot,view,match,profile}.c` | **Bách** | Task 6–9 và phần `profileCompute` của Task 11. Sơn review ba điểm tích hợp ranked đã bàn giao và đoạn tính profile delta trước finalize. |
| Python integration, fixtures và nghiệm thu LAN | **Cả hai** | Task 15. Sơn chạy và tổng hợp suite, Bách xác nhận các ca ván đấu/profile. Sơn giữ `requests.json`, Bách giữ `views.json`; đợt này cả hai dùng lại dữ liệu hiện có. |

Nguồn phân công: [thiết kế §6](../specs/2026-11-11-ubuntu-tcp-design.md#6-lưu-trữ-và-phân-công), [plan tổng Task 1–15](2026-11-11-ubuntu-tcp-uno.md), [plan hạ tầng của Sơn](2026-11-15-tcp-server-network-storage.md), [plan sửa review TCP](2026-11-15-fix-tcp-listener-review.md), [plan tích hợp ngày 25/11](2026-11-25-son-task9-12-10-11.md) và [bàn giao Sơn → Bách](../handoff-son-to-bach-2026-11-25.md).

## Cấu trúc đích

```text
server/
  main.c, app.{c,h}                             # Sơn · CLI, poll loop, dispatch
  auth/{accounts,auth_worker}.{c,h}             # Sơn · tài khoản và xác thực
  game/{game,bot,view,match,profile}.{c,h}       # Bách · luật, view, vòng đời trận, profile delta
  game/rating.{c,h}                             # Sơn · tính ELO và hạng
  lobby/{rooms,queue,social}.{c,h}               # Sơn · phòng, party, queue, presence
  storage/{store.c,store.h,schema.sql}          # Sơn · SQLite và giao dịch
shared/{contracts.h,protocol.c,protocol.h,net.c,net.h}
desktop/{main.c,bridge.c,bridge.h,tcp_client.c,tcp_client.h,net_session.c,net_session.h}
tests/
  unit/test_*.c                                 # 16 file C, giữ tên test và chủ sở hữu
  integration/                                  # Python, giữ package import
  fixtures/                                     # JSON và HTML dùng bởi unit/smoke test
```

`main.c` và `app.*` là điểm vào server. `shared/` và `desktop/` đủ nhỏ để tìm theo tên file. Đặt `profile.*` cạnh Matches vì nó tính delta từ kết quả trận. `social.*` xử lý request hồ sơ, `store.*` đọc/ghi SQLite. `rating.*` nằm gần finalize do Matches gọi khi chốt trận, Sơn tiếp tục sở hữu file. Cây thư mục cải thiện locality khi tìm mã và test. Các seam và interface hiện có giữ nguyên ngoài việc đổi tên C theo convention.

## Task 0 — Chốt baseline · Sơn và Bách

- [ ] Bách hoàn tất và tích hợp phần đang sửa ở `match.c`, `profile.*` và hai test C tương ứng, hoặc giữ riêng patch và cùng Sơn chọn một SHA làm baseline. Ghi rõ SHA và hai bài Python Task 15 nào đã vào baseline. Chạy cùng một tập ca trước và sau refactor.
- [ ] Sơn chạy `make -B server desktop`, `make -B test`, `make integration`, `make sanitize` và `make desktop-smoke` từ repo root trên Ubuntu. Ghi exit code, số ca chạy và lỗi có sẵn nếu có. So sánh kết quả sau mỗi đợt, giữ các thay đổi nghiệp vụ trong PR riêng.
- [ ] Mỗi người làm việc từ cùng SHA sau khi chốt baseline. Thống nhất phần đổi tên interface ở Task 3 trước khi sửa bất kỳ caller nào của người kia.

## Đợt 1 — Chuẩn hóa convention trên đường dẫn hiện tại

Task 1 và 2 có thể làm song song với phần định dạng, comment và tên dùng nội bộ từng file. Tên xuất hiện trong header hoặc được module khác gọi sẽ đổi cùng caller ở Task 3.

### Task 1 — Game, bot, view, match và profile · Bách

**File** — `server/{game,bot,view,match,profile}.{c,h}` và `tests/test_{game,modes,bot,view,match,profile}.c`.

- [ ] Chuẩn hóa 4 spaces, dấu `{` cùng dòng với hàm/câu điều khiển, tách dòng quá dài. Tên biến và hàm nội bộ dùng `camelCase`; hằng số dùng `UPPER_SNAKE_CASE`. Giữ trị số, kiểu, thứ tự thao tác và assertion.
- [ ] Rà comment đầu hàm theo mục đích, tham số, giá trị trả về và điều kiện quan trọng. Ưu tiên làm rõ sở hữu JSON của view, clock monotonic cho timer, điều kiện retry finalize và thời điểm tính `profileCompute`. Comment trong thân hàm giải thích lý do hoặc invariant.
- [ ] Chạy vòng lặp bên dưới. Kiểm tra `prepare → persist → publish`, thứ tự penalty/timeout, view theo ghế, ELO và profile delta qua các test có sẵn. Sơn review điểm gọi Store và rating.

```bash
for name in game modes bot view match profile; do make test NAME="$name" || exit 1; done
```

### Task 2 — TCP, account, lobby, SQLite và desktop native · Sơn

**File** — C/header còn lại và các test C tương ứng trong bảng phân công. `Makefile` và schema chỉ cần sửa khi Task 4 chuyển đường dẫn.

- [ ] Áp dụng cùng checklist format, tên và comment cho implementation, header và test. Đổi tên socket theo vai trò, ví dụ `listenSocket`, `connectionSocket`, `clientAddress`, `bytesReceived`, `bytesSent`. Giữ phân biệt fd và ID logic của connection. Đặt `LISTEN_BACKLOG = 128` thay cho đối số trực tiếp ở `listen()`; giữ nguyên các ngưỡng khác khi đặt tên.
- [ ] Ở `server/app.c`, `shared/net.c`, `desktop/tcp_client.c`, xác nhận mọi kết quả `socket/bind/listen/accept/recv/send` đều có cách xử lý. Giữ byte order, EOF, retry `EINTR`, xử lý `EAGAIN/EWOULDBLOCK`, partial I/O, cleanup và quy tắc non-blocking. Nhánh đầy kết nối trong `app.c` hiện bỏ qua kết quả `send()`. Xử lý riêng kết quả âm/ghi thiếu của lần gửi `SERVER_FULL`, ví dụ ghi chẩn đoán ngắn khi gửi thiếu hoặc lỗi, rồi đóng socket ngay như hiện tại. Để diff này riêng khỏi đổi tên/format và giữ nguyên cơ chế từ chối best-effort.
- [ ] Rà comment đầu hàm cho caller, gồm lifetime của buffer/JSON, sở hữu fd, thứ tự poll và worker, SQLite transaction, backoff/RESUME và việc token chỉ nằm ở native.
- [ ] Chạy vòng lặp bên dưới và `make integration NAME=transport`. Kiểm tra test `test_max_connections` vẫn nhận `SERVER_FULL` trong trường hợp gửi bình thường. Bách review framing, command/push và phần seam native ↔ JS.

```bash
for name in protocol net accounts rooms queue rating social store net_session bridge; do
    make test NAME="$name" || exit 1
done
make integration NAME=transport
```

### Task 3 — Đồng bộ tên C qua các module · Sơn tích hợp, Bách sửa phần mình

**File** — Header, implementation, caller, callback và test dùng tên được đổi ở Task 1–2. Tên interface đã đúng convention như `matchesHandle` và `profileCompute` được dùng tiếp.

- [ ] Sau khi ghép Task 1 và 2, rà tên còn lại trong header và lập bảng cũ → mới theo từng file. Ví dụ `frame_feed → frameFeed`, `conn_flush → connFlush`, `app_now_ms → appNowMs`, `store_finalize → storeFinalize`, `rating_apply → ratingApply`, `client_connect_start → clientConnectStart`. Dùng chuyển tên cơ học, giữ chữ ký, giá trị enum, layout struct và contract. Với tên field như `Conn.out_buf`, chỉ thay identifier C cùng các initializer/caller tương ứng. Giữ `main`, tên thư viện và literal wire/SQL/CLI.
- [ ] Mỗi người soạn phần diff của các file mình sở hữu trên cùng baseline sau Task 1–2. Sơn ghép thành một PR build được, Bách review hunk cross-owner. Với mỗi tên cũ, tìm toàn bộ caller trong C/header và đối chiếu docs giao thức nếu có trích tên C. Giữ literal wire và dữ liệu fixtures y như baseline.
- [ ] Từ repo root, chạy `make -B server desktop`, `make -B test`, `make integration` và `git diff --check`. Kết thúc Đợt 1 khi hunk còn lại chỉ là convention, chú thích và kiểm tra kết quả network đã nêu.

## Đợt 2 — Chuyển file và cập nhật build

### Task 4 — Đưa source và test về cây đích · Sơn tích hợp, Bách phụ trách gameplay

- [ ] Dựa trên SHA đã qua Đợt 1, Bách chuyển năm cặp file gameplay và sáu test C của mình. Sơn chuyển các cặp auth/lobby/storage, `rating.*`, `schema.sql` và mười test C còn lại. Cùng tích hợp trong một PR. Giữ lịch sử rename dễ đọc bằng commit chuyển file trước, sau đó cập nhật include/build trong cùng PR.
- [ ] File `.c` include header của chính nó theo basename. Với header của module khác, dùng path từ repo root, ví dụ `"server/storage/store.h"`, `"server/game/match.h"`, `"shared/protocol.h"`. Sửa mọi include cũ trong server, desktop và tests. Giữ header guards; danh sách phụ thuộc gồm header cả trong thư mục con.
- [ ] Sơn sửa `Makefile` ở các biến liệt kê source/header và rule test. Liệt kê `server`, `server/auth`, `server/game`, `server/lobby`, `server/storage` bằng glob một cấp theo từng thư mục. Chỉ link `server/main.c` và `desktop/main.c` vào binary tương ứng. Unit tests đọc source từ `tests/unit/test_*.c`, vẫn tạo `build/tests/test_*`. `test_net` vẫn link `desktop/tcp_client.c`, `test_net_session` vẫn link `desktop/net_session.c`, `test_bridge` tiếp tục dùng GTK/WebKit và desktop modules.
- [ ] Giữ `tests/fixtures/` cho unit test và `desktop-smoke`, `tests/integration/` cho Python imports và `build/uno-server` cho `ServerProcess`. `store.c` dùng schema nhúng lúc chạy. Khi chuyển `schema.sql`, giữ nguyên nội dung và cơ chế migration. Cập nhật tài liệu đang sử dụng nếu có link tới path đã chuyển. Các plan cũ tiếp tục làm mốc lịch sử.
- [ ] Kiểm tra `git diff --summary -M` trên diff của PR và `git diff --check`. Soát include cũ bằng `rg`, build từ repo root, chạy lại test C lẫn Python. Giữ `make test NAME=protocol`, `make test NAME=net_session` và `make integration NAME=match` hoạt động qua cùng lệnh.

### Task 5 — Nghiệm thu · Sơn và Bách

Chạy trên Ubuntu từ repo root. Dùng `BUILD` mặc định cho integration vì `ServerProcess` tìm `build/uno-server`.

```bash
make BUILD=build/refactor-headless HAVE_GTK= server test
make -B server desktop test
make integration
make sanitize
make desktop-smoke
(cd client && npm ci && npm run build)
make package
tar -tzf build/uno-ubuntu.tar.gz
```

- [ ] Sơn xác nhận headless chạy 15 test C và bản có GTK chạy đủ 16 nếu baseline đủ các file trên. Lệnh build, test, integration, sanitizer và smoke trả exit code 0, hoặc ghi rõ lỗi baseline đã xác nhận. Archive chứa hai binary, `client/dist` và `docs/ubuntu-demo.md`.
- [ ] Bách so sánh fixtures và test trận theo baseline. Rà ranked/unranked/aborted, reconnect giữ ghế, chốt trận chỉ một lần và profile delta. Nếu hai bài Task 15 đã vào baseline, chạy cả `make integration NAME=full_flow` và `make integration NAME=resilience` để ghi kết quả riêng.
- [ ] Hai người chạy demo LAN trên desktop build mới, dùng UI hiện có để đăng nhập, chơi một trận, ngắt rồi nối mạng và kiểm tra RESUME. Ghi thiết bị đã dùng, kết quả và phần còn thiếu nếu chưa tái hiện đủ ca LAN trong handoff.
- [ ] Soát diff theo baseline bằng `git diff --name-status <baseline>...HEAD`. Cây đích có đủ 55 file C/header, 16 test C có chủ sở hữu. `git diff --exit-code <baseline>...HEAD -- client/` trả 0. Các thay đổi ngoài source phục vụ đường dẫn, build hoặc tài liệu refactor.

## Các ca dễ gãy cần đối chiếu

| Ca cần kiểm tra | Test hoặc lệnh hiện có |
|---|---|
| Frame chia nhỏ/dính nhau, ghi socket một phần và server đầy | `make test NAME=protocol`, `make test NAME=net`, `make integration NAME=transport` |
| Relogin/RESUME giữ đúng ghế đang chơi, không gửi lại action cũ | `make integration NAME=match`, `make test NAME=net_session` |
| View theo ghế và token chỉ ở native | `make test NAME=view`, `make test NAME=bridge`, `make integration NAME=match` |
| Mở DB cũ, lỗi ghi/rollback, finalize lặp và ELO sau restart | `make test NAME=store`, `make test NAME=match`, `make integration NAME=ranking` |
| Vẫn tìm thấy asset và test sau khi chuyển path | `make -B test`, `make desktop-smoke`, `make package` |

**Thứ tự ghép** — Task 0 → Task 1 và 2 song song → Task 3 → Task 4 → Task 5. Sơn là người duy nhất sửa build chung. Bách là người sửa chính `match.*`; Sơn cùng review các điểm nối dispatch, ELO và Store.
