# Sơn — Task 4: đưa module server và unit test về cây thư mục đích

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Chuyển các module server của Sơn vào `server/auth/`, `server/lobby/`, `server/storage/`, `server/game/` và 10 unit test C vào `tests/unit/`. Cập nhật include và `Makefile` để build, test C và integration chạy như trước.

**Architecture:** Mỗi đợt chuyển gồm hai commit. Commit đầu chỉ có `git mv`, để Git ghi nhận rename 100%. Commit sau sửa include và `Makefile`. Không đổi tên C, chữ ký hay nội dung hàm. Mọi include tuân theo một quy tắc duy nhất: file `.c` include header của chính nó theo basename, mọi header khác của dự án theo path từ repo root. Bỏ `-Ishared` để build tự bắt include sai quy tắc.

**Tech Stack:** C11, GNU make, json-c, SQLite3, libsodium, GTK3/WebKitGTK 4.1 (chỉ desktop và `test_bridge`), Python 3 unittest.

**Spec:** [Kế hoạch refactor tổng](2026-11-25-c-convention-folder-refactor.md), mục "Cấu trúc đích" và "Task 4".

**Branch:** `refactor/server-module-layout`, tạo từ `main` @ `d331058` (đã có Task 1–3).

## Global Constraints

- Commit message **không** có dòng `Co-Authored-By`. Tiền tố: `refactor(c):` cho commit chuyển file, `build:` cho commit sửa include/Makefile.
- Build `-std=c11 -Wall -Wextra -Wpedantic -Werror`. Server/test headless phải build khi không có GTK.
- Không đổi nội dung file ngoài dòng `#include`. Riêng `Makefile` chỉ đổi biến liệt kê source/header/test, rule `test_bridge` và cờ `-I`.
- Giữ `tests/fixtures/`, `tests/integration/`, `build/uno-server`, `build/tests/test_*`, tên target `make test NAME=<x>`, `make integration NAME=<x>`.
- `server/storage/schema.sql` giữ nguyên nội dung. `store.c` vẫn dùng schema nhúng, không đọc file này.
- Không động vào `client/`. Plan, spec, review và handoff cũ giữ nguyên làm mốc lịch sử; `ARCHITECTURE.md` mô tả thiết kế cũ (epoll/WebSocket) nên cũng giữ nguyên.

## Cây đích và bảng chuyển file

| Cũ | Mới | Chủ |
|---|---|---|
| `server/accounts.{c,h}`, `server/auth_worker.{c,h}` | `server/auth/` | Sơn |
| `server/rooms.{c,h}`, `server/queue.{c,h}`, `server/social.{c,h}` | `server/lobby/` | Sơn |
| `server/store.{c,h}`, `server/schema.sql` | `server/storage/` | Sơn |
| `server/rating.{c,h}` | `server/game/` | Sơn |
| `tests/test_{protocol,net,accounts,rooms,queue,rating,social,store,net_session,bridge}.c` | `tests/unit/` | Sơn |

Giữ nguyên chỗ: `server/main.c`, `server/app.{c,h}`, `server/game/{game,bot,view,match,profile}.*` (Bách đã chuyển), `shared/*`, `desktop/*`.

## Quy tắc include

```c
// server/lobby/rooms.c
#include "rooms.h"                    // own header: basename
#include "server/app.h"               // everything else: repo-root path
#include "server/auth/accounts.h"
#include "server/storage/store.h"
#include "shared/protocol.h"
```

Header chỉ include header khác bằng path từ repo root (ví dụ `shared/net.h` → `"shared/protocol.h"`). Một header không có "header của chính nó", nên không dùng basename.

Lệnh kiểm tra quy tắc (dùng ở Task 1 và Task 2, in ra mọi include sai, không in gì khi đúng):

```bash
for f in $(git ls-files 'server/*.c' 'server/*.h' 'shared/*.c' 'shared/*.h' 'desktop/*.c' 'desktop/*.h' 'tests/*.c'); do
    own="$(basename "${f%.*}").h"
    grep -oE '^#include "[^"]+"' "$f" | sed -E 's/#include "(.*)"/\1/' | while read -r inc; do
        if [ "${f##*.}" = c ] && [ "$inc" = "$own" ] && [ -f "$(dirname "$f")/$inc" ]; then continue; fi
        case "$inc" in */*) [ -f "$inc" ] && continue ;; esac
        echo "$f: \"$inc\""
    done
done
```

## Môi trường kiểm chứng

Working tree Windows có CRLF (`core.autocrlf=true`), nên `make` chạy trong bản sao LF ở WSL Ubuntu-22.04. Mỗi lần kiểm: tạo patch phía Windows bằng `git diff HEAD --binary > <scratchpad>/wip.patch`. Sau đó trong WSL chạy `git fetch <repo> refactor/server-module-layout && git reset --hard FETCH_HEAD` ở `~/uno-verify`, rồi `git apply wip.patch` nếu patch khác rỗng. Không chạy `git diff` bằng git của WSL trên `/mnt/f`: nó coi mọi file CRLF là đã sửa. Các bước "Run" dưới đây ghi lệnh chạy bên trong `~/uno-verify`. Nghiệm thu trên Ubuntu 24.04 thuộc Task 5 của plan tổng.

## Review Focus

1. **Header cũ còn được tìm thấy qua đường vòng.** Một include cũ như `"store.h"` có thể vẫn build nhờ `-I` hoặc nhờ file cùng thư mục. Bỏ `-Ishared` và chạy lệnh kiểm tra include ở Task 1/2 để bắt trường hợp này.
2. **Sửa header đã chuyển mà không rebuild.** `headers` trong `Makefile` phải glob cả thư mục mới, nếu không `make` dùng binary cũ. Task 1 Step 7 kiểm bằng `touch`.
3. **Chạy một test theo tên sau khi chuyển.** `make test NAME=store` phải tìm thấy `tests/unit/test_store.c` qua `vpath`. Task 2 chạy từng tên.
4. **Build headless.** Không có GTK thì bỏ `test_bridge`, vẫn đủ 15 test. Task 3 chạy `HAVE_GTK=`.
5. **Integration vẫn tìm `build/uno-server`.** `ServerProcess` dùng path cố định. Task 3 chạy đủ `make integration`.

---

### Task 0: Ghi baseline

**Files:** không sửa file nào.

- [ ] **Step 1: Đồng bộ bản sao WSL về `d331058` và chạy bộ kiểm.**

Run:
```bash
git log -1 --format=%h
make -B server desktop; echo "build $?"
make -B test; echo "unit $?"
make BUILD=build/refactor-headless HAVE_GTK= server test; echo "headless $? $(ls build/refactor-headless/tests | wc -l)"
make integration 2>&1 | grep -E '^Ran|^OK|FAILED'
make sanitize > /dev/null 2>&1; echo "sanitize $?"
make desktop-smoke 2>&1 | tail -1
```
Expected: `d331058`; build, unit, sanitize trả 0; headless 15 test; integration `Ran 56 tests`, `OK`; `desktop-smoke: OK`. Ghi kết quả vào mô tả PR. Nếu khác, ghi lỗi baseline trước khi sửa gì.

---

### Task 1: Chuyển module server và sửa include

**Files:**
- Move: bảng chuyển file ở trên (11 file `server/`).
- Modify include: `server/app.{c,h}`, `server/main.c`, mọi file trong `server/auth/`, `server/lobby/`, `server/storage/`, `server/game/rating.c`, `server/game/match.{c,h}`, `server/game/profile.c`, `shared/net.h`, `shared/protocol.h`, `desktop/main.c`, `desktop/net_session.c`, `tests/test_*.c`, `tests/unit/test_{match,profile}.c`.
- Modify: `Makefile` (dòng `CPPFLAGS`, `server_dirs`).

**Interfaces:**
- Consumes: không.
- Produces: header ở path mới `server/auth/accounts.h`, `server/auth/auth_worker.h`, `server/lobby/rooms.h`, `server/lobby/queue.h`, `server/lobby/social.h`, `server/storage/store.h`, `server/game/rating.h`. Task 2 và mọi caller include theo các path này.

- [ ] **Step 1: Chuyển file bằng `git mv`.**

```bash
mkdir -p server/auth server/lobby server/storage
git mv server/accounts.c server/accounts.h server/auth_worker.c server/auth_worker.h server/auth/
git mv server/rooms.c server/rooms.h server/queue.c server/queue.h server/social.c server/social.h server/lobby/
git mv server/store.c server/store.h server/schema.sql server/storage/
git mv server/rating.c server/rating.h server/game/
```

- [ ] **Step 2: Commit phần chuyển file.**

```bash
git commit -m "refactor(c): move server modules into auth, lobby and storage"
```

Build ở commit này chưa chạy được. Commit sau trong cùng PR sửa lại.

- [ ] **Step 3: Chạy lệnh kiểm tra include để thấy lỗi.**

Expected: danh sách khác rỗng, có ít nhất `server/app.c: "accounts.h"`, `server/game/match.c: "server/store.h"`, `shared/net.h: "protocol.h"`, `desktop/main.c: "bridge.h"` và `tests/test_protocol.c: "protocol.h"`.

- [ ] **Step 4: Sửa include theo quy tắc.**

Áp bảng thay thế cho mọi file liệt kê ở **Files**:

| Include cũ (basename hoặc path cũ) | Include mới |
|---|---|
| `"accounts.h"`, `"server/accounts.h"` | `"server/auth/accounts.h"` |
| `"auth_worker.h"` | `"server/auth/auth_worker.h"` |
| `"rooms.h"`, `"server/rooms.h"` | `"server/lobby/rooms.h"` |
| `"queue.h"`, `"server/queue.h"` | `"server/lobby/queue.h"` |
| `"social.h"`, `"server/social.h"` | `"server/lobby/social.h"` |
| `"store.h"`, `"server/store.h"` | `"server/storage/store.h"` |
| `"rating.h"`, `"server/rating.h"` | `"server/game/rating.h"` |
| `"app.h"` (ngoài `app.c`) | `"server/app.h"` |
| `"protocol.h"`, `"contracts.h"` trong `shared/*.h` | `"shared/protocol.h"`, `"shared/contracts.h"` |
| `"bridge.h"`, `"net_session.h"`, `"tcp_client.h"` trong `desktop/main.c` | `"desktop/bridge.h"`, `"desktop/net_session.h"`, `"desktop/tcp_client.h"` |
| `"protocol.h"` trong `tests/test_protocol.c` | `"shared/protocol.h"` |

Ngoại lệ theo quy tắc: file `.c` giữ header của chính nó theo basename. Ví dụ `accounts.c` giữ `"accounts.h"`, `rating.c` đổi `"server/rating.h"` thành `"rating.h"`, `net_session.c` đổi `"desktop/net_session.h"` thành `"net_session.h"`. Giữ thứ tự include hiện có trong từng file, chỉ thay chuỗi.

Các dòng trong `server/game/match.{c,h}`, `server/game/profile.c`, `tests/unit/test_{match,profile}.c` là file của Bách: chỉ đổi path include, ghi tên file vào mô tả PR để Bách review.

- [ ] **Step 5: Sửa `Makefile`.**

```make
CPPFLAGS += -I. $(shell pkg-config --cflags json-c sqlite3 libsodium)
server_dirs := server server/auth server/game server/lobby server/storage
```

Bỏ `-Ishared`, vì sau Step 4 không include nào cần nó. Nếu còn file cần, build ở Step 6 sẽ báo.

- [ ] **Step 6: Chạy lại lệnh kiểm tra include rồi build và test.**

Run: lệnh kiểm tra include, sau đó
```bash
make -B server desktop && make -B test
```
Expected: lệnh kiểm tra không in gì. Build exit 0, 16 test C exit 0.

- [ ] **Step 7: Kiểm tra dependency header mới.**

Run: `make server >/dev/null && touch server/storage/store.h && make -n server | grep -c uno-server`
Expected: `1` hoặc lớn hơn, tức `uno-server` được build lại khi header ở thư mục mới đổi.

- [ ] **Step 8: Commit.**

```bash
git add Makefile server shared desktop tests
git commit -m "build: include server modules from their new directories"
```

---

### Task 2: Chuyển 10 unit test C về `tests/unit/`

**Files:**
- Move: `tests/test_{protocol,net,accounts,rooms,queue,rating,social,store,net_session,bridge}.c` → `tests/unit/`.
- Modify: `Makefile` (`test_dirs`, rule `test_bridge`).

**Interfaces:**
- Consumes: path header từ Task 1.
- Produces: mọi unit test C nằm ở `tests/unit/test_*.c`, binary vẫn là `$(BUILD)/tests/test_<name>`.

- [ ] **Step 1: Chuyển file và commit.**

```bash
git mv tests/test_protocol.c tests/test_net.c tests/test_accounts.c tests/test_rooms.c \
       tests/test_queue.c tests/test_rating.c tests/test_social.c tests/test_store.c \
       tests/test_net_session.c tests/test_bridge.c tests/unit/
git commit -m "refactor(c): move remaining unit tests into tests/unit"
```

- [ ] **Step 2: Chạy `make -B test` để thấy lỗi.**

Expected: lỗi `No rule to make target 'tests/test_bridge.c', needed by 'build/tests/test_bridge'`, vì rule GTK vẫn trỏ path cũ. Các test khác vẫn được tìm qua `vpath`.

- [ ] **Step 3: Sửa `Makefile`.**

`test_dirs := tests/unit` và rule GTK thành `$(BUILD)/tests/test_bridge: tests/unit/test_bridge.c $(desktop_modules) $(shared) $(headers) Makefile`.

- [ ] **Step 4: Kiểm tra include và chạy từng test theo tên.**

Run: lệnh kiểm tra include, sau đó
```bash
ls tests/*.c 2>/dev/null | wc -l
for name in protocol net accounts rooms queue rating social store net_session bridge game modes bot view match profile; do
    make test NAME="$name" > /dev/null || { echo "FAIL $name"; exit 1; }
done; echo all-16-ok
```
Expected: lệnh kiểm tra không in gì; `0` file C còn ở `tests/`; in `all-16-ok`.

- [ ] **Step 5: Commit.**

```bash
git add Makefile
git commit -m "build: build unit tests from tests/unit"
```

---

### Task 3: Kiểm tra toàn bộ và soát diff

**Files:** không sửa code. Ghi kết quả vào mô tả PR.

- [ ] **Step 1: Chạy toàn bộ bộ kiểm như Task 0.**

Run: các lệnh ở Task 0 Step 1, cộng thêm
```bash
make test NAME=protocol && make test NAME=net_session && make integration NAME=match
```
Expected: cùng kết quả với baseline: build/unit/sanitize 0, headless 15 test, integration 56/56 OK, `desktop-smoke: OK`. Ba lệnh theo tên trả 0.

- [ ] **Step 2: Soát rename và include cũ.**

Run:
```bash
git diff --summary -M main...HEAD | grep -c 'rename .*(100%)'
git diff --check main...HEAD && echo check-clean
git grep -nE '#include "(server/)?(accounts|auth_worker|rooms|queue|social|store|rating)\.h"' -- server shared desktop tests
git diff --stat main...HEAD -- client | tail -1
git log main..HEAD --format=%B | grep -ci co-authored
```
Expected: `21` rename 100% (11 file server, 10 test); `check-clean`. `git grep` chỉ in các include basename của chính file (ví dụ `server/auth/accounts.c: #include "accounts.h"`), không có path cũ. `client` không đổi. `0` dòng co-author.

- [ ] **Step 3: Soát số file theo cây đích.**

Run: `git ls-files 'server/*.c' 'server/*.h' 'shared/*.c' 'shared/*.h' 'desktop/*.c' 'desktop/*.h' 'tests/unit/*.c' | wc -l`, `git ls-files 'tests/unit/*.c' | wc -l` và `git ls-files 'tests/*.c' | grep -vc '^tests/unit/'`
Expected: `55` (39 file C/header nguồn và 16 unit test, bằng `d331058`), `16`, `0`.

- [ ] **Step 4: Viết mô tả PR `refactor/server-module-layout → main`.**

Nội dung: baseline và kết quả Step 1, bảng chuyển file, quy tắc include, danh sách dòng include đã đổi trong file của Bách (`server/game/match.{c,h}`, `server/game/profile.c`, `tests/unit/test_{match,profile}.c`), việc bỏ `-Ishared`. Ghi rõ Task 5 (Ubuntu 24.04, `make package`, demo LAN) còn chờ. Mô tả PR không có dòng `Co-Authored-By`. Chỉ push và mở PR khi Sơn đồng ý.
