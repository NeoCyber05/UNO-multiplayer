# Phòng tổ đội (2 vs 2 / Side to Side) + Trang tổng kết trận

Ngày: 2026-09-30 · Trạng thái: đã duyệt

## Mục tiêu

1. Bấm 2 vs 2 / Side to Side ở Sảnh → vào **phòng tổ đội** trước. Phòng hiện
   danh sách bạn bè online/offline; bạn online có thể được mời làm đồng đội.
2. Sau mỗi trận (mọi chế độ) → **trang tổng kết** riêng.
3. Ở trang tổng kết có thể **gửi lời mời kết bạn** tới người chơi chưa là bạn.

Chưa có backend: bạn bè, trạng thái online, phản hồi lời mời, người lạ khi ghép
trận đều là dữ liệu giả lập.

## Luồng

```
Sảnh → 2v2/Side → /room/:mode (phòng tổ đội)
   ├─ Mời bạn online → "Đã mời…" ~2s → vào ghế đồng đội (bạn đang trong trận → từ chối)
   ├─ Ấn "Đã mời…" để huỷ; nút ✕ trên ghế để mời đồng đội ra
   └─ "Tìm trận" (có hoặc không có đồng đội)
        → /matchmaking/:mode: giữ ghế đồng đội, điền ghế trống bằng người lạ (STRANGERS)
        → /game/:mode (đội hình thật, ranked)
        → hết ván ~1.5s → /result
             ├─ Chơi tiếp: ranked → /matchmaking/:mode (giữ party); custom → /custom
             ├─ Về phòng (chỉ 2v2/Side ranked) → /room/:mode
             └─ Về sảnh
```

Classic vẫn đi thẳng ghép trận (không phòng). Custom truyền đội hình (bạn + bot) vào
bàn chơi với `ranked: false`.

## Kiến trúc

- `context/SocialContext.jsx`: `me` (ELO cập nhật trong phiên), `friends`,
  `sendFriendRequest(person)`, `friendState(id)` → `'friend' | 'pending' | 'none'`,
  `party = { mode, teammate }`, `lastMatch`, `applyElo(delta)`.
- `data/mockData.js`: thêm `STRANGERS` (id `s-*`, có ELO).
- `pages/TeamRoom.jsx`: phòng tổ đội.
- `pages/MatchResult.jsx`: trang tổng kết; bỏ `ResultModal`.
- `game/engine.js`: `initGame(modeKey, lineup?)`; `lineup` = 4 người theo ghế
  (0 bạn, 1 trái, 2 trên, 3 phải). Không truyền → đội hình bot cũ. Mỗi player có
  `stats = { played, attacks, unoCalls, penalties }`.
- `game/elo.js`: Elo chuẩn, K = 32; so ELO trung bình đội mình vs đội kia
  (Classic: bạn vs trung bình 3 người còn lại).
- `game/summary.js`: `buildMatchSummary(state, { ranked, now })` → `lastMatch`.
- `lobbyModes.js`: 2v2 → `/room/2v2`, side → `/room/side`.
- `FriendsModal.jsx`: dùng context (kết bạn đồng bộ với trang tổng kết).

## Phòng tổ đội

- Ghế đồng đội = `MODES[mode].teammateSeat` (2v2: trên, Side: trái). 2 ghế đối thủ
  nét đứt "Ghép trận sẽ tìm", không mời được.
- Danh sách bạn: online trước, offline mờ, không hiện lời mời kết bạn đang chờ.
- Nút mỗi bạn: offline → không có nút; rảnh → **Mời**; đang chờ → **Đã mời…**
  (ấn = huỷ); là đồng đội → **Trong phòng** (disabled); đã có đồng đội hoặc
  đang chờ người khác → Mời disabled.
- Timer mời dọn khi huỷ / unmount.
- Mode không phải `2v2`/`side` → redirect `/matchmaking/classic`.

## Trang tổng kết

- Không có `lastMatch` (F5) → redirect Sảnh.
- Banner CHIẾN THẮNG / THUA RỒI, chế độ, thời lượng.
- Ranked: ELO cũ → mới, ±delta, bậc hạng. Custom: "Không tính ELO".
- Team mode: 2 cột theo đội, đội thắng viền vàng. Classic: 1 danh sách xếp theo
  điểm bài còn lại.
- Mỗi dòng: avatar, tên, số lá / điểm, lá đã đánh, +2/+4 đã ném, hô UNO.
- Nút kết bạn (trừ bạn và bot): đã là bạn → chip "Bạn bè"; đã gửi → "Đã gửi ✓";
  còn lại → **+ Kết bạn**. Không giả lập chấp nhận.

## Test

Thêm `vitest`; test `elo.js`, `summary.js`, `initGame` với lineup (team đúng ghế) và
đếm stats trong reducer. UI kiểm bằng `npm run build` + chạy dev.
