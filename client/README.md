# UNO Online — Client (ReactJS + Vite)

## Chạy thử

```bash
cd client
npm install
npm run dev      # mở http://localhost:5173
npm run build    # build production vào dist/
```

Luồng màn hình: `/` Đăng nhập → `/lobby` Sảnh (carousel chọn chế độ) →
Classic: `/matchmaking/classic` Ghép trận → `/game/classic` Bàn chơi (tính ELO);
2 vs 2 / Side to Side: `/room/:mode` Phòng tổ đội (mời 1 bạn online làm đồng đội) →
`/matchmaking/:mode` (tìm đối thủ là người lạ) → `/game/:mode` (tính ELO);
Custom: `/custom` Phòng tùy chỉnh (mời bạn / thêm bot) → `/game/:mode` (không tính ELO).
Hết ván → `/result` Tổng kết (ELO ±, chỉ số từng người, nút kết bạn, chơi tiếp / về phòng / về sảnh).
Bạn bè, nhóm tổ đội, ELO trong phiên, kết quả trận: `src/context/SocialContext.jsx`.
Test logic (engine, ELO, tổng kết): `npm test`.
`:mode` = `classic` | `2v2` | `side`. Thêm chế độ lên carousel: sửa `src/data/lobbyModes.js`.

Bàn chơi đã chơi được thật với 3 bot (engine luật UNO chạy local):
đánh bài, rút bài, Skip / Reverse / +2 / Wild / +4, chọn màu, hô UNO (quên hô bị phạt +2),
đếm giờ lượt 20 giây, chat nhanh, màn hình kết quả.

## Cấu trúc

```
src/
  config/assets.js        ← KHAI BÁO ẢNH Ở ĐÂY
  data/mockData.js        Người chơi / bạn bè mẫu
  game/                   Engine luật chơi (thuần JS, tách khỏi UI)
    engine.js             reducer: PLAY, DRAW, PASS, SAY_UNO, ...
    bot.js                AI cho bot
    useUnoGame.js         hook nối engine với React (bot, hẹn giờ)
  components/
    card/UnoCard.jsx      Lá bài (ảnh)
    game/*                Các phần của bàn chơi
  pages/                  6 màn hình
  styles/theme.css        Biến màu, nút, input, modal dùng chung
```

## Hình ảnh cần thêm

> 📖 **Xem hướng dẫn chi tiết đầy đủ quy chuẩn, kích thước, danh sách 54 lá bài và prompt AI tại:** [IMAGE_SPECIFICATIONS.md](./IMAGE_SPECIFICATIONS.md)

Nền bàn chơi, nền menu, lá bài, thẻ chế độ **bắt buộc có ảnh** (không còn bản vẽ CSS),
sinh bằng script trong `scripts/`: `gen_cards.py`, `gen_modes.py`, `gen_table_bg.py`,
`gen_menu_bg.py`. Logo, nền đăng nhập, avatar, nút UNO vẫn tùy chọn: copy ảnh vào
`client/public/images/...` rồi điền đường dẫn trong `src/config/assets.js`
(để `null` = giữ bản vẽ CSS).

| Mục | Khoá trong `ASSETS` | Thư mục gợi ý | Kích thước gợi ý |
|---|---|---|---|
| Nền bàn chơi (nền đỏ, đèn sân khấu, chữ UNO mờ) | `backgrounds.table` | `images/backgrounds/table.jpg` | 1440×900 trở lên (16:10) |
| Nền các màn hình menu | `backgrounds.menu` | `images/backgrounds/menu.jpg` | 1440×900 |
| Nền cột trái màn đăng nhập | `backgrounds.login` | `images/backgrounds/login.jpg` | 640×900 |
| Logo game | `logo` | `images/ui/logo.png` | ~400×160, PNG trong suốt |
| Mặt sau lá bài | `cardBack` | `images/cards/back.png` | 240×360 (tỉ lệ 2:3) |
| Mặt trước 54 loại lá | `cardFace` (hàm) | `images/cards/{color}_{value}.png` | 240×360 (tỉ lệ 2:3) |
| Ảnh đại diện người chơi | `avatars['p-me']`, ... | `images/avatars/*.png` | 256×256 (vuông) |
| Nút CALL UNO | `unoButton` | `images/ui/uno-button.png` | 320×320, PNG trong suốt |

Tên file mặt bài theo `cardFace: (card) => \`/images/cards/${card.color}_${card.value}.png\``:

- màu: `red`, `yellow`, `green`, `blue` × giá trị `0`–`9`, `skip`, `reverse`, `draw2`
  (ví dụ `red_7.png`, `blue_skip.png`, `green_draw2.png`)
- lá đen: `wild_wild.png`, `wild_wild4.png`

> Lưu ý bản quyền: mặt bài / logo UNO chính hãng thuộc Mattel. Nếu phát hành công khai
> nên dùng hình tự thiết kế.

## Bước tiếp theo (multiplayer)

`useUnoGame` đang chạy reducer local. Khi có server (Socket.io / Supabase Realtime):
chạy `gameReducer` ở server, client gửi action (`PLAY`, `DRAW`, `SAY_UNO`, ...) và
nhận state đã lọc (ẩn bài của người khác) để render — các component UI giữ nguyên.
