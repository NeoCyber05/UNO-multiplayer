# UNO Online

App nằm trong `client/` (React + Vite).

## Cấu trúc repo

```
client/                   App: React + Vite, có engine luật chơi UNO
docs/                     Spec / plan thiết kế
```

## Chạy app (client)

```bash
cd client
npm install
npm run dev      # http://localhost:5173
npm run build    # build production vào client/dist/
```

Chi tiết cấu trúc, engine luật chơi, danh sách ảnh cần thêm: xem
[client/README.md](./client/README.md).

## Trạng thái hiện tại

- Bàn chơi đã chơi được thật với 3 bot: đánh bài, rút bài, Skip/Reverse/+2,
  Wild/+4 chọn màu, hô UNO (quên hô bị phạt +2), đếm giờ lượt, chat nhanh, màn
  hình kết quả. Engine luật chơi chạy local (`client/src/game/`).
- Luồng màn hình: Đăng nhập → Sảnh (carousel thẻ chế độ) → Ghép trận (Classic /
  2 vs 2 / Side to Side) hoặc Phòng Custom (mời bạn, thêm bot) → Bàn chơi.
- ELO, bảng xếp hạng, độ khó bot hiện chỉ là giao diện với dữ liệu giả.
- Chưa có: đăng nhập thật (chưa có backend/tài khoản), phòng/ghép trận
  multiplayer thật (cần server + realtime), đồng bộ nhiều người chơi qua
  mạng — hiện engine chỉ chạy trong 1 trình duyệt với bot.

## Gợi ý bước tiếp theo

1. Chọn stack backend cho realtime (Node/Express + Socket.io, hoặc Supabase
   Realtime) cho phần đăng nhập, phòng, ghép trận.
2. Chuyển engine luật chơi UNO (`client/src/game/engine.js`) lên chạy ở
   server, client chỉ gửi action (`PLAY`, `DRAW`, `SAY_UNO`, ...) và nhận
   state đã lọc (ẩn bài người khác) để render.
3. Thêm các ảnh còn thiếu (logo, nền đăng nhập, avatar, nút UNO) theo hướng dẫn
   trong [client/IMAGE_SPECIFICATIONS.md](./client/IMAGE_SPECIFICATIONS.md).
