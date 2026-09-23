# Thiết kế lại Sảnh: carousel chọn chế độ + phòng Custom

Ngày: 2026-09-29 · Trạng thái: chờ duyệt

## Mục tiêu

Thay sảnh kiểu dashboard (sidebar trái) bằng sảnh kiểu game: các chế độ chơi
là thẻ lá bài nằm trong carousel ở giữa màn hình, tham khảo giao diện UNO trên
điện thoại nhưng tối ưu cho PC (chuột + bàn phím, khung 1440×900).

Phạm vi: **chỉ giao diện**. Không đổi engine luật chơi, không thêm backend,
không thêm logic độ khó bot, không có ELO thật (dùng dữ liệu giả).

Ngoài phạm vi: cửa hàng, sự kiện, tiền tệ, hồ sơ chi tiết, chế độ Xếp hạng
riêng.

## Luồng màn hình

```
Đăng nhập → Sảnh (carousel)
              ├─ Classic ──────┐
              ├─ 2 vs 2 ───────┼→ Ghép trận (/matchmaking/:mode) → Bàn chơi (/game/:mode)   tính ELO
              ├─ Side to Side ─┘
              └─ Custom → Phòng tùy chỉnh (/custom) → Bàn chơi (/game/:rule)             không tính ELO
```

Route sau thay đổi:

| Route | Trang | Ghi chú |
|---|---|---|
| `/` | Login | không đổi |
| `/lobby` | Lobby | viết lại |
| `/custom` | CustomRoom | mới, thay `/room/:mode` |
| `/matchmaking/:mode` | Matchmaking | nút Quay lại → `/lobby` |
| `/game/:mode` | GameTable | nhận `classic`, `2v2`, `side` |
| `/mode`, `/room/:mode` | — | xóa |

Đổi key chế độ `solo` → `classic` (label "Classic", 4 người mỗi người một mình)
trong `game/constants.js` và mọi nơi dùng. `Matchmaking` chấp nhận cả
`classic` (hiện đang loại `solo`).

## Màn Sảnh (`/lobby`)

Bố cục (khung 1440×900):

- **Thanh trên**
  - Trái: thẻ hồ sơ — avatar, tên, 🏆 ELO, cấp + thanh XP. Bấm → toast
    "Hồ sơ: sắp ra mắt".
  - Giữa: logo UNO (`ASSETS.logo` nếu có, không thì chữ).
  - Phải: nút âm thanh (bật/tắt, chỉ đổi icon), nút cài đặt (toast "sắp ra mắt").
- **Carousel** ở giữa, chiếm phần lớn chiều cao.
- **Khối thông tin** dưới carousel cho thẻ đang chọn: tên, mô tả, chip,
  nút CHƠI NGAY, dãy chấm vị trí.
- **Dock** căn giữa đáy: BXH, Luật chơi.
- Góc dưới phải: gợi ý phím mờ "← → chọn · Enter chơi".

### Danh sách chế độ (`data/lobbyModes.js`)

Mảng cấu hình, thêm chế độ mới = thêm một phần tử:

```js
{ key: 'classic', title: 'Classic', desc: '...', chip: 'XẾP HẠNG · ELO',
  ranked: true, color: '#E8212E', image: '/images/modes/classic.png', to: '/matchmaking/classic' }
```

| key | title | chip | to |
|---|---|---|---|
| `2v2` | 2 vs 2 | XẾP HẠNG · ELO | `/matchmaking/2v2` |
| `classic` | Classic | XẾP HẠNG · ELO | `/matchmaking/classic` |
| `side` | Side to Side | XẾP HẠNG · ELO | `/matchmaking/side` |
| `custom` | Custom | KHÔNG TÍNH ELO | `/custom` |

Thẻ mặc định ở giữa khi mở sảnh: `classic`.

### Thẻ chế độ (`components/lobby/ModeCard.jsx`)

- Thẻ là **ảnh** (`<img src={mode.image}>`), tỉ lệ 2:3, ảnh tự chứa toàn bộ
  thiết kế (viền trắng, chữ tên mode...). Không vẽ lá bài bằng CSS.
- Nếu ảnh lỗi/chưa có (`onError`): hiện khung bo góc nền `mode.color` với tên
  mode ở giữa. Chỉ là placeholder, không trang trí thêm.
- Ảnh đặt tại `client/public/images/modes/<key>.png`, gợi ý 600×900 PNG.

### Carousel (`components/lobby/ModeCarousel.jsx`)

- Xoay vòng (index modulo số thẻ). Vị trí tương đối `offset = i - active`
  chuẩn hóa về khoảng gần 0 nhất.
- Hiển thị: `offset 0` to nhất + viền sáng; `±1` nhỏ hơn; `±2` nhỏ hơn nữa và mờ,
  chỉ hiện khi có từ 5 thẻ trở lên (tránh một thẻ xuất hiện hai phía). Thẻ khác ẩn.
- Vị trí/kích thước bằng `transform` + `transition` CSS theo offset
  (biến CSS `--offset`), không dùng thư viện.
- Tương tác:
  - Bấm thẻ bên → xoay tới thẻ đó. Bấm thẻ giữa → vào chế độ.
  - Mũi tên ‹ › hai bên.
  - Phím ← → đổi thẻ, Enter vào chế độ (listener trên `window`, gỡ khi unmount;
    bỏ qua khi đang mở modal hoặc focus trong input).
  - Cuộn chuột: đổi 1 thẻ mỗi lần, chặn lặp (throttle ~250ms).

### Dock

- **BXH** → `LeaderboardModal`: danh sách hạng, avatar, tên, ELO từ
  `mockData.LEADERBOARD`; dòng của mình được tô sáng.
- **Luật chơi** → `RulesModal`: luật cơ bản, lá chức năng, khác biệt
  Classic / 2 vs 2 / Side to Side, cách tính ELO (mô tả chung).
- Dùng lại `components/Modal.jsx` có sẵn.

## Màn Custom (`/custom`)

Viết lại từ `Room.jsx`, dùng lại phần bàn + ghế + mã phòng.

- **Header**: nút quay lại (`/lobby`), tiêu đề "Phòng tùy chỉnh", nút
  "Nhập mã phòng" (modal nhập 4 ký tự → toast "Không tìm thấy phòng" vì chưa
  có server), mã phòng bấm để copy.
- **Tab luật**: Classic / 2 vs 2 / Side to Side. Đổi tab → ghế tô màu đội theo
  `LAYOUT` hiện có (Classic: không chia đội).
- **4 ghế**:
  - Ghế 0: bạn, chip "Chủ phòng".
  - Ghế trống: nút "Mời bạn" (mở/tập trung danh sách bạn bè) và "+ Bot".
  - Ghế bot: tên "Bot", chọn độ khó Dễ / Thường / Khó (`<select>` nhỏ), nút ✕ để bỏ.
  - Ghế bạn bè: avatar, tên, chip đồng đội/đối thủ (khi có đội), nút ✕.
- **Cột phải**:
  - Danh sách bạn bè (online có nút Mời → vào ghế trống đầu tiên, giả lập nhận
    ngay; offline mờ, không mời được).
  - Nút "Thêm bot vào mọi ô trống".
  - Nút BẮT ĐẦU (n/4), bật khi đủ 4 ghế → `/game/<rule>`.
- Độ khó bot chỉ lưu trong state của trang, **không** truyền vào engine.

## Dữ liệu giả (`data/mockData.js`)

- `ME.elo`, `elo` cho bạn bè.
- `LEADERBOARD`: ~10 người, có cả `ME`.
- `BOT_LEVELS = ['Dễ', 'Thường', 'Khó']`.

## Ảnh (`config/assets.js`, `IMAGE_SPECIFICATIONS.md`)

- Ảnh mode không cần khai báo trong `ASSETS` — đường dẫn nằm trong
  `lobbyModes.js`.
- Bổ sung `IMAGE_SPECIFICATIONS.md`: mục "Thẻ chế độ" (600×900 PNG, tỉ lệ 2:3,
  bo góc có sẵn trong ảnh, nền trong suốt ngoài góc bo) + prompt gợi ý cho từng
  mode để tự tạo bằng công cụ AI.

## Dọn dẹp

- Xóa `pages/ModeSelect.jsx`, `pages/Room.jsx`.
- Xóa CSS không còn dùng trong `pages.css`: sidebar, `lobby__*`, `play-hero`,
  `featured`, `mode-card`, `quick-rank`, `friend*` (nếu CustomRoom không dùng lại).
- Style Sảnh mới trong `components/lobby/lobby.css`.
- Cập nhật `README.md` phần luồng màn hình.

## Kiểm tra

Project chưa có test tự động. Kiểm tra bằng:

1. `npm run build` không lỗi/cảnh báo mới.
2. Chạy `npm run dev`, mở `/lobby`: carousel xoay bằng chuột, phím, cuộn;
   Enter vào đúng route; placeholder hiện khi thiếu ảnh; BXH, Luật chơi mở/đóng.
3. `/custom`: đổi tab, thêm/bỏ bot, đổi độ khó, mời bạn, bắt đầu khi đủ 4.
4. Classic / 2 vs 2 / Side → ghép trận → bàn chơi chạy được với key `classic`.
