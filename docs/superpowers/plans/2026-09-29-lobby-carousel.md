# Lobby Carousel + Custom Room Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Thay sảnh kiểu sidebar bằng sảnh kiểu game (carousel thẻ chế độ bằng ảnh), thêm phòng Custom (mời bạn/thêm bot), bỏ trang chọn chế độ và phòng chờ cũ.

**Architecture:** React + Vite client, không đổi engine. Sảnh ghép từ các component nhỏ trong `src/components/lobby/` (TopBar, ModeCarousel, ModeCard, Dock, 2 modal), dữ liệu chế độ khai báo trong `src/data/lobbyModes.js`. Phòng Custom là trang mới `src/pages/CustomRoom.jsx` tái dùng CSS bàn/ghế `room-*` có sẵn trong `pages.css`. Key chế độ `solo` đổi thành `classic`.

**Tech Stack:** React 18, react-router-dom 6, Vite 5, CSS thuần (biến CSS trong `styles/theme.css`).

**Spec:** `docs/superpowers/specs/2026-09-29-lobby-carousel-design.md`

---

## Ghi chú chung

- Mọi lệnh chạy trong `client/` (`cd "D:/CODE C/UNO-multiplayer/client"`).
- Project **chưa có test tự động** và spec chỉ yêu cầu giao diện → không thêm test framework. Mỗi task kiểm bằng `npm run build` (phải kết thúc bằng `✓ built in ...`, không có lỗi). Task cuối kiểm tay trên trình duyệt.
- Thư mục **chưa phải git repo** → bước "Checkpoint" = build pass. Nếu người dùng đã `git init` thì commit ở mỗi checkpoint với message ghi trong bước.
- Khung thiết kế cố định 1440×900 (`components/Stage.jsx`), dùng px tuyệt đối như code hiện có.
- Class dùng chung có sẵn trong `styles/theme.css`: `.screen`, `.btn`, `.btn--primary|ghost|outline|danger|sm|block`, `.icon-btn`, `.panel`, `.input`, `.label`, `.chip`, `.dot`, `.avatar`, `.modal`, `.modal__box`, `.menu-toast`, `.display`, `.muted`.

## Cấu trúc file

| File | Loại | Trách nhiệm |
|---|---|---|
| `src/game/constants.js` | sửa | key `solo` → `classic` |
| `src/game/engine.js` | sửa | fallback `MODES.classic` |
| `src/pages/GameTable.jsx` | sửa | fallback `'classic'` |
| `src/pages/Matchmaking.jsx` | sửa | nhận `classic`, quay lại `/lobby` |
| `src/data/mockData.js` | sửa | `elo`, `LEADERBOARD`, `BOT_LEVELS` |
| `src/data/lobbyModes.js` | tạo | danh sách chế độ trên carousel |
| `src/components/Icons.jsx` | sửa | thêm `IconSound`, `IconMute`, `IconBook` |
| `src/components/lobby/ModeCard.jsx` | tạo | ảnh thẻ + placeholder khi thiếu ảnh |
| `src/components/lobby/ModeCarousel.jsx` | tạo | xoay vòng, chuột, phím, cuộn |
| `src/components/lobby/TopBar.jsx` | tạo | hồ sơ + ELO, logo, âm thanh/cài đặt |
| `src/components/lobby/Dock.jsx` | tạo | hàng nút dưới đáy |
| `src/components/lobby/LeaderboardModal.jsx` | tạo | popup BXH |
| `src/components/lobby/RulesModal.jsx` | tạo | popup luật chơi |
| `src/components/lobby/lobby.css` | tạo | style Sảnh |
| `src/pages/Lobby.jsx` | viết lại | ghép Sảnh |
| `src/pages/CustomRoom.jsx` | tạo | phòng tùy chỉnh |
| `src/App.jsx` | sửa | route `/custom`, bỏ `/mode`, `/room/:mode` |
| `src/pages/ModeSelect.jsx`, `src/pages/Room.jsx` | xóa | thay bởi carousel + CustomRoom |
| `src/pages/pages.css` | sửa | xóa CSS sidebar/lobby/mode cũ, thêm CSS Custom |
| `public/images/modes/.gitkeep` | tạo | thư mục ảnh thẻ |
| `IMAGE_SPECIFICATIONS.md`, `README.md`, `../README.md` | sửa | tài liệu |

---

### Task 1: Đổi key chế độ `solo` → `classic`

**Files:**
- Modify: `src/game/constants.js:12`
- Modify: `src/game/engine.js:30`
- Modify: `src/pages/GameTable.jsx:177`
- Modify: `src/pages/Matchmaking.jsx:15`, nút Quay lại và Huỷ tìm trận

- [ ] **Step 1: Sửa `constants.js`**

Thay dòng:
```js
  solo: { key: 'solo', label: '1 Người', accent: '#4C86FF', teams: false, teammateSeat: null, teammateOpen: false },
```
bằng:
```js
  classic: { key: 'classic', label: 'Classic', accent: '#E8212E', teams: false, teammateSeat: null, teammateOpen: false },
```

- [ ] **Step 2: Sửa `engine.js`**

```js
  const cfg = MODES[modeKey] ?? MODES.solo;
```
→
```js
  const cfg = MODES[modeKey] ?? MODES.classic;
```

- [ ] **Step 3: Sửa `GameTable.jsx`**

```js
  const modeKey = MODES[mode] ? mode : 'solo';
```
→
```js
  const modeKey = MODES[mode] ? mode : 'classic';
```

- [ ] **Step 4: Sửa `Matchmaking.jsx`**

```js
  const cfg = MODES[mode] && mode !== 'solo' ? MODES[mode] : MODES['2v2'];
```
→
```js
  const cfg = MODES[mode] ?? MODES.classic;
```

Và thay **cả hai** chỗ `navigate(`/room/${cfg.key}`)` (nút Quay lại trong header và nút "Huỷ tìm trận") thành `navigate('/lobby')`.

- [ ] **Step 5: Kiểm tra không còn `solo`**

Run: `grep -rn "solo" src --include=*.js --include=*.jsx`
Expected: chỉ còn các dòng trong `src/pages/ModeSelect.jsx` (file này bị xóa ở Task 7).

- [ ] **Step 6: Checkpoint**

Run: `npm run build`
Expected: `✓ built in ...`, không lỗi.
(git: `refactor: rename solo mode to classic`)

---

### Task 2: Dữ liệu — ELO, BXH, độ khó bot, danh sách chế độ

**Files:**
- Modify: `src/data/mockData.js`
- Create: `src/data/lobbyModes.js`
- Create: `public/images/modes/.gitkeep`

- [ ] **Step 1: Sửa `mockData.js`**

Thay dòng `export const ME = ...` bằng:
```js
export const ME = { id: 'p-me', name: 'Bách', initials: 'BC', tint: '#4C86FF', level: 12, xp: 68, elo: 1491 };
```

Thay khối `export const FRIENDS = [...]` bằng:
```js
export const FRIENDS = [
  { ...BOTS[2], elo: 1620, status: 'Đang ở sảnh chờ', online: true },
  { ...BOTS[1], elo: 1555, status: 'Đang trong trận · 2 vs 2', online: true },
  { ...BOTS[0], elo: 1402, status: 'Rảnh', online: true },
  { id: 'p-ql', name: 'Quốc Long', initials: 'QL', tint: '#FFC94A', elo: 1338, status: 'Offline 2 giờ trước', online: false },
];

// Bảng xếp hạng ELO (đã sắp giảm dần). Sau này lấy từ server.
export const LEADERBOARD = [
  { id: 'lb-1', name: 'Khánh Linh', initials: 'KL', tint: '#FF5262', elo: 2140 },
  { id: 'lb-2', name: 'Đức Anh', initials: 'ĐA', tint: '#4C86FF', elo: 2055 },
  { id: 'lb-3', name: 'Phương Vy', initials: 'PV', tint: '#35D399', elo: 1987 },
  { id: 'lb-4', name: 'Tuấn Kiệt', initials: 'TK', tint: '#FFC94A', elo: 1902 },
  { id: 'lb-5', name: 'Gia Hân', initials: 'GH', tint: '#B073FF', elo: 1811 },
  { id: 'lb-6', name: 'Minh Quân', initials: 'MQ', tint: '#FF8A3D', elo: 1740 },
  FRIENDS[0],
  FRIENDS[1],
  ME,
  FRIENDS[2],
];

export const BOT_LEVELS = ['Dễ', 'Thường', 'Khó'];
```
(Giữ nguyên `BOTS` và `QUICK_CHAT`. `FRIENDS` phải khai báo trước `LEADERBOARD`.)

- [ ] **Step 2: Tạo `src/data/lobbyModes.js`**

```js
// Các chế độ hiện trên carousel ở Sảnh. Thêm chế độ mới = thêm 1 phần tử.
// image: ảnh thẻ PNG 600x900 (tỉ lệ 2:3) trong public/images/modes/.
//        Thiếu ảnh -> hiện khung màu `color` kèm tên chế độ.
export const LOBBY_MODES = [
  {
    key: '2v2',
    title: '2 vs 2',
    desc: 'Đồng đội ngồi đối diện qua bàn, bài của nhau luôn hiện ra để phối hợp nước đi.',
    chip: 'XẾP HẠNG · ELO',
    ranked: true,
    color: '#1FA64A',
    image: '/images/modes/2v2.png',
    to: '/matchmaking/2v2',
  },
  {
    key: 'classic',
    title: 'Classic',
    desc: 'UNO truyền thống: 4 người, mỗi người một mình, ai hết bài trước thắng.',
    chip: 'XẾP HẠNG · ELO',
    ranked: true,
    color: '#E8212E',
    image: '/images/modes/classic.png',
    to: '/matchmaking/classic',
  },
  {
    key: 'side',
    title: 'Side to Side',
    desc: 'Đồng đội ngồi ngay cạnh bạn, mỗi người giữ bài riêng, đối thủ ngồi hàng đối diện.',
    chip: 'XẾP HẠNG · ELO',
    ranked: true,
    color: '#7B3FE4',
    image: '/images/modes/side.png',
    to: '/matchmaking/side',
  },
  {
    key: 'custom',
    title: 'Custom',
    desc: 'Phòng riêng: mời bạn bè hoặc thêm bot, tự chọn luật chơi.',
    chip: 'KHÔNG TÍNH ELO',
    ranked: false,
    color: '#F0A800',
    image: '/images/modes/custom.png',
    to: '/custom',
  },
];

// Thẻ nằm giữa khi mở Sảnh
export const DEFAULT_MODE = 'classic';
```

- [ ] **Step 3: Tạo thư mục ảnh**

Run: `mkdir -p public/images/modes && touch public/images/modes/.gitkeep`

- [ ] **Step 4: Checkpoint**

Run: `npm run build`
Expected: `✓ built in ...`.
(git: `feat: add lobby modes config and mock ELO data`)

---

### Task 3: Icon mới

**Files:**
- Modify: `src/components/Icons.jsx` (thêm sau `IconClose`)

- [ ] **Step 1: Thêm 3 icon**

Chèn ngay sau khối `export const IconClose = ...` (trước `SkipGlyph`):
```jsx
export const IconSound = ({ size = 20 }) => (
  <svg {...base(size, 2.1)}><path d="M4 9v6h4l5 4V5L8 9H4z" /><path d="M16.5 8.5a5 5 0 010 7M19 6a8.5 8.5 0 010 12" /></svg>
);
export const IconMute = ({ size = 20 }) => (
  <svg {...base(size, 2.1)}><path d="M4 9v6h4l5 4V5L8 9H4z" /><path d="M17 9.5l5 5M22 9.5l-5 5" /></svg>
);
export const IconBook = ({ size = 22 }) => (
  <svg {...base(size, 2)}><path d="M4 5a2 2 0 012-2h13v16H6a2 2 0 00-2 2V5z" /><path d="M4 19a2 2 0 012-2h13" /><path d="M9 7h6" /></svg>
);
```

- [ ] **Step 2: Checkpoint**

Run: `npm run build` → `✓ built in ...`.
(git: `feat: add sound, mute, book icons`)

---

### Task 4: ModeCard + ModeCarousel

**Files:**
- Create: `src/components/lobby/ModeCard.jsx`
- Create: `src/components/lobby/ModeCarousel.jsx`
- Create: `src/components/lobby/lobby.css` (phần carousel; Task 5–6 nối thêm)

- [ ] **Step 1: Tạo `ModeCard.jsx`**

```jsx
import { useState } from 'react';

/** Thẻ chế độ = ảnh lá bài. Thiếu ảnh / ảnh lỗi -> khung màu trơn có tên chế độ. */
export default function ModeCard({ mode }) {
  const [broken, setBroken] = useState(false);

  if (broken || !mode.image) {
    return (
      <div className="mode-card-fallback" style={{ background: mode.color }}>
        <span className="display">{mode.title}</span>
      </div>
    );
  }
  return (
    <img
      className="mode-card-img"
      src={mode.image}
      alt={mode.title}
      draggable={false}
      onError={() => setBroken(true)}
    />
  );
}
```

- [ ] **Step 2: Tạo `ModeCarousel.jsx`**

```jsx
import { useEffect, useRef } from 'react';
import { IconArrowRight, IconBack } from '../Icons.jsx';
import ModeCard from './ModeCard.jsx';

const WHEEL_GAP_MS = 250;

// Khoảng cách vòng ngắn nhất từ `active` tới `i` (vd n=4, active=0, i=3 -> -1).
function offsetOf(i, active, n) {
  let d = (i - active) % n;
  if (d < 0) d += n;
  if (d > n / 2) d -= n;
  return d;
}

/**
 * Carousel xoay vòng các thẻ chế độ.
 * - Bấm thẻ bên -> xoay tới; bấm thẻ giữa -> onSelect.
 * - Phím ← → đổi thẻ, Enter chọn (tắt khi keyboard=false, vd đang mở modal).
 * - Cuộn chuột đổi 1 thẻ mỗi lần.
 * Hiện thẻ giữa ± 1; từ 5 thẻ trở lên hiện thêm ± 2.
 */
export default function ModeCarousel({ modes, active, onChange, onSelect, keyboard = true }) {
  const n = modes.length;
  const reach = n >= 5 ? 2 : 1;
  const lastWheel = useRef(0);

  const go = (step) => onChange((active + step + n) % n);

  useEffect(() => {
    if (!keyboard) return undefined;
    const onKey = (e) => {
      if (e.target.closest?.('input, textarea, select')) return;
      if (e.key === 'ArrowLeft') onChange((active - 1 + n) % n);
      else if (e.key === 'ArrowRight') onChange((active + 1) % n);
      else if (e.key === 'Enter' && !e.repeat) {
        // Nút khác (dock, modal...) đang focus thì để nút đó tự xử lý Enter
        if (e.target.closest?.('button:not(.carousel__item)')) return;
        e.preventDefault(); // chặn click mặc định của thẻ đang focus -> không vào 2 lần
        onSelect(modes[active]);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [keyboard, active, n, modes, onChange, onSelect]);

  const onWheel = (e) => {
    const delta = Math.abs(e.deltaY) >= Math.abs(e.deltaX) ? e.deltaY : e.deltaX;
    const now = Date.now();
    if (Math.abs(delta) < 4 || now - lastWheel.current < WHEEL_GAP_MS) return;
    lastWheel.current = now;
    go(delta > 0 ? 1 : -1);
  };

  return (
    // preventDefault ở mousedown: bấm chuột không giữ focus trên thẻ/mũi tên,
    // nên phím ← → Enter luôn áp dụng cho thẻ đang ở giữa.
    <div className="carousel" onWheel={onWheel} onMouseDown={(e) => e.preventDefault()}>
      <button type="button" className="carousel__arrow carousel__arrow--left" tabIndex={-1} onClick={() => go(-1)} aria-label="Chế độ trước">
        <IconBack size={28} />
      </button>

      <div className="carousel__track">
        {modes.map((m, i) => {
          const off = offsetOf(i, active, n);
          const abs = Math.abs(off);
          const hidden = abs > reach;
          return (
            <button
              key={m.key}
              type="button"
              className={`carousel__item ${off === 0 ? 'is-active' : ''} ${hidden ? 'is-hidden' : ''}`}
              style={{ '--off': off, '--abs': abs, zIndex: 10 - abs }}
              tabIndex={hidden ? -1 : 0}
              aria-hidden={hidden}
              aria-label={off === 0 ? `Chơi ${m.title}` : m.title}
              onClick={() => (off === 0 ? onSelect(m) : onChange(i))}
            >
              <ModeCard mode={m} />
            </button>
          );
        })}
      </div>

      <button type="button" className="carousel__arrow carousel__arrow--right" tabIndex={-1} onClick={() => go(1)} aria-label="Chế độ tiếp">
        <IconArrowRight size={28} />
      </button>
    </div>
  );
}
```

- [ ] **Step 3: Tạo `lobby.css` (phần carousel)**

```css
/* =========================================================
   SẢNH (LOBBY) — carousel chế độ
   ========================================================= */
.carousel { position: relative; width: 100%; height: 100%; }
.carousel__track { position: absolute; inset: 0; }

.carousel__item {
  position: absolute;
  left: 50%;
  top: 50%;
  width: 300px;
  height: 450px;
  padding: 0;
  border: none;
  border-radius: 22px;
  background: none;
  box-shadow: 0 18px 40px rgba(0, 0, 0, 0.45);
  transform:
    translate(-50%, -50%)
    translateX(calc(var(--off) * 285px))
    scale(calc(1 - var(--abs) * 0.2))
    rotate(calc(var(--off) * 3deg));
  opacity: calc(1 - var(--abs) * 0.2);
  filter: brightness(calc(1 - var(--abs) * 0.18));
  transition: transform 0.45s cubic-bezier(0.2, 1, 0.3, 1), opacity 0.3s, filter 0.3s, box-shadow 0.3s;
}
.carousel__item:not(.is-active):hover { filter: brightness(1); }
.carousel__item.is-active {
  box-shadow: 0 0 0 5px #ffc94a, 0 0 50px rgba(255, 201, 74, 0.45), 0 24px 50px rgba(0, 0, 0, 0.55);
}
.carousel__item.is-active:hover {
  box-shadow: 0 0 0 6px #ffd76e, 0 0 70px rgba(255, 201, 74, 0.6), 0 28px 56px rgba(0, 0, 0, 0.6);
}
.carousel__item.is-hidden { opacity: 0; pointer-events: none; }

.mode-card-img,
.mode-card-fallback {
  display: block;
  width: 100%;
  height: 100%;
  border-radius: 22px;
  object-fit: cover;
  user-select: none;
}
.mode-card-fallback {
  display: flex;
  align-items: center;
  justify-content: center;
  border: 8px solid #fff;
  font-size: 38px;
  font-weight: 700;
  color: #fff;
  text-shadow: 0 3px 0 rgba(0, 0, 0, 0.35);
}

.carousel__arrow {
  position: absolute;
  top: 50%;
  z-index: 20;
  width: 58px;
  height: 58px;
  transform: translateY(-50%);
  border: none;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(255, 255, 255, 0.14);
  color: #fff;
  transition: background 0.2s, transform 0.15s;
}
.carousel__arrow:hover { background: rgba(255, 255, 255, 0.26); transform: translateY(-50%) scale(1.08); }
.carousel__arrow--left { left: 32px; }
.carousel__arrow--right { right: 32px; }
```

- [ ] **Step 4: Checkpoint**

Run: `npm run build` → `✓ built in ...` (component chưa được import nên chỉ kiểm cú pháp ở Task 6).
(git: `feat: add mode card and carousel components`)

---

### Task 5: TopBar, Dock, LeaderboardModal, RulesModal

**Files:**
- Create: `src/components/lobby/TopBar.jsx`
- Create: `src/components/lobby/Dock.jsx`
- Create: `src/components/lobby/LeaderboardModal.jsx`
- Create: `src/components/lobby/RulesModal.jsx`
- Modify: `src/components/lobby/lobby.css` (nối thêm)

- [ ] **Step 1: Tạo `TopBar.jsx`**

```jsx
import Avatar from '../Avatar.jsx';
import { IconGear, IconMute, IconSound, IconTrophy } from '../Icons.jsx';
import { ASSETS } from '../../config/assets';

/** Thanh trên Sảnh: thẻ hồ sơ (bấm mở hồ sơ), logo, nút âm thanh + cài đặt. */
export default function TopBar({ me, muted, onToggleMute, onProfile, onSettings }) {
  return (
    <header className="topbar">
      <button type="button" className="topbar__profile" onClick={onProfile} aria-label="Hồ sơ">
        <Avatar person={me} size={58} shape="square" />
        <span className="topbar__info">
          <span className="topbar__row">
            <span className="topbar__name">{me.name}</span>
            <span className="topbar__elo">
              <IconTrophy size={15} /> {me.elo.toLocaleString('vi-VN')}
            </span>
          </span>
          <span className="topbar__xp"><i style={{ width: `${me.xp}%` }} /></span>
          <span className="topbar__lv">Cấp {me.level} · {me.xp}% XP</span>
        </span>
      </button>

      <div className="topbar__logo">
        {ASSETS.logo ? <img src={ASSETS.logo} alt="UNO" /> : <span className="display">UNO</span>}
      </div>

      <div className="topbar__actions">
        <button type="button" className="topbar__btn" onClick={onToggleMute} aria-label={muted ? 'Bật âm thanh' : 'Tắt âm thanh'}>
          {muted ? <IconMute size={22} /> : <IconSound size={22} />}
        </button>
        <button type="button" className="topbar__btn" onClick={onSettings} aria-label="Cài đặt">
          <IconGear size={22} />
        </button>
      </div>
    </header>
  );
}
```

- [ ] **Step 2: Tạo `Dock.jsx`**

```jsx
/** Hàng nút dưới đáy Sảnh. items: [{ key, label, icon, onClick }] */
export default function Dock({ items }) {
  return (
    <nav className="dock">
      {items.map(({ key, label, icon: Icon, onClick }) => (
        <button key={key} type="button" className="dock__item" onClick={onClick}>
          <span className="dock__icon"><Icon size={28} /></span>
          <span className="dock__label">{label}</span>
        </button>
      ))}
    </nav>
  );
}
```

- [ ] **Step 3: Tạo `LeaderboardModal.jsx`**

```jsx
import Modal from '../Modal.jsx';
import Avatar from '../Avatar.jsx';
import { IconClose, IconTrophy } from '../Icons.jsx';
import { LEADERBOARD, ME } from '../../data/mockData';

export default function LeaderboardModal({ open, onClose }) {
  return (
    <Modal open={open} onClose={onClose} label="Bảng xếp hạng" className="lobby-modal">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconTrophy size={24} /> Bảng xếp hạng ELO</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>
      <ol className="lb-list">
        {LEADERBOARD.map((p, i) => (
          <li key={p.id} className={`lb-row ${p.id === ME.id ? 'is-me' : ''} ${i < 3 ? `is-top-${i + 1}` : ''}`}>
            <span className="lb-row__rank display">{i + 1}</span>
            <Avatar person={p} size={36} />
            <span className="lb-row__name">{p.id === ME.id ? `${p.name} (Bạn)` : p.name}</span>
            <span className="lb-row__elo display">{p.elo.toLocaleString('vi-VN')}</span>
          </li>
        ))}
      </ol>
    </Modal>
  );
}
```

- [ ] **Step 4: Tạo `RulesModal.jsx`**

```jsx
import Modal from '../Modal.jsx';
import { IconBook, IconClose } from '../Icons.jsx';

const SECTIONS = [
  {
    title: 'Mục tiêu',
    text: 'Đánh hết bài trên tay trước người khác. Khi còn 1 lá phải bấm UNO, quên hô sẽ bị phạt rút 2 lá.',
  },
  {
    title: 'Lượt đánh',
    text: 'Đánh 1 lá cùng màu, cùng số hoặc cùng ký hiệu với lá trên cùng. Không có lá hợp lệ thì rút 1 lá. Mỗi lượt có 20 giây.',
  },
  {
    title: 'Lá chức năng',
    text: 'Skip: người kế mất lượt. Reverse: đảo chiều. +2: người kế rút 2 lá. Discard All: bỏ luôn mọi lá cùng màu trên tay. Wild: đổi màu. Wild +4: đổi màu và người kế rút 4 lá.',
  },
  {
    title: 'Classic',
    text: '4 người, mỗi người một mình. Người hết bài đầu tiên thắng.',
  },
  {
    title: '2 vs 2',
    text: 'Hai đội, đồng đội ngồi đối diện và luôn nhìn thấy bài của nhau. Một người trong đội hết bài là cả đội thắng.',
  },
  {
    title: 'Side to Side',
    text: 'Hai đội, đồng đội ngồi cạnh nhau, mỗi người giữ bài riêng.',
  },
  {
    title: 'ELO',
    text: 'Classic, 2 vs 2 và Side to Side là trận xếp hạng: thắng được cộng ELO, thua bị trừ. Phòng Custom không tính ELO.',
  },
];

export default function RulesModal({ open, onClose }) {
  return (
    <Modal open={open} onClose={onClose} label="Luật chơi" className="lobby-modal">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconBook size={24} /> Luật chơi</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>
      <div className="rules">
        {SECTIONS.map((s) => (
          <section key={s.title} className="rules__item">
            <h3 className="display">{s.title}</h3>
            <p>{s.text}</p>
          </section>
        ))}
      </div>
    </Modal>
  );
}
```

- [ ] **Step 5: Nối CSS vào cuối `lobby.css`**

```css
/* ---------- Top bar ---------- */
.topbar {
  position: relative;
  z-index: 5;
  display: grid;
  grid-template-columns: 1fr auto 1fr;
  align-items: center;
  padding: 26px 40px 0;
}
.topbar__profile {
  justify-self: start;
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 8px 20px 8px 8px;
  border: none;
  border-radius: 18px;
  text-align: left;
  color: #3a2200;
  background: linear-gradient(180deg, #ffd35a, #f0a800);
  box-shadow: 0 4px 0 #a86e00, 0 12px 26px rgba(0, 0, 0, 0.3);
  transition: transform 0.15s, filter 0.2s;
}
.topbar__profile:hover { transform: translateY(-2px); filter: brightness(1.05); }
.topbar__profile .avatar { border: 3px solid #fff; }
.topbar__info { display: flex; flex-direction: column; gap: 4px; min-width: 170px; }
.topbar__row { display: flex; align-items: center; justify-content: space-between; gap: 14px; }
.topbar__name { font-weight: 800; font-size: 16px; }
.topbar__elo { display: inline-flex; align-items: center; gap: 4px; font-family: var(--font-display); font-weight: 600; font-size: 15px; }
.topbar__xp { display: block; height: 7px; border-radius: 99px; background: rgba(0, 0, 0, 0.22); overflow: hidden; }
.topbar__xp i { display: block; height: 100%; border-radius: inherit; background: linear-gradient(90deg, #ff8a3d, #e3301c); }
.topbar__lv { font-size: 11px; font-weight: 700; opacity: 0.8; }

.topbar__logo span {
  display: block;
  font-size: 58px;
  font-weight: 700;
  line-height: 1;
  color: #ffd21f;
  -webkit-text-stroke: 3px #4a1400;
  text-shadow: 0 5px 0 #4a1400;
  transform: rotate(-5deg);
}
.topbar__logo img { max-height: 80px; display: block; }

.topbar__actions { justify-self: end; display: flex; gap: 12px; }
.topbar__btn {
  width: 50px;
  height: 50px;
  border: none;
  border-radius: 16px;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #fff;
  background: linear-gradient(180deg, rgba(255, 255, 255, 0.22), rgba(255, 255, 255, 0.08));
  transition: background 0.2s, transform 0.15s;
}
.topbar__btn:hover { background: rgba(255, 255, 255, 0.28); transform: scale(1.06); }

/* ---------- Dock ---------- */
.dock { position: relative; z-index: 5; display: flex; justify-content: center; gap: 28px; padding: 0 0 26px; }
.dock__item {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 0;
  border: none;
  background: none;
  color: #fff;
}
.dock__icon {
  width: 62px;
  height: 62px;
  border-radius: 20px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(180deg, rgba(255, 255, 255, 0.24), rgba(255, 255, 255, 0.08));
  box-shadow: 0 8px 20px rgba(0, 0, 0, 0.3);
  transition: transform 0.2s, background 0.2s;
}
.dock__item:hover .dock__icon { transform: translateY(-4px); background: rgba(255, 255, 255, 0.3); }
.dock__label { font-size: 12px; font-weight: 800; letter-spacing: 0.06em; text-transform: uppercase; }

/* ---------- Modal Sảnh (BXH, Luật chơi) ---------- */
.lobby-modal { width: 560px; max-height: 760px; display: flex; flex-direction: column; gap: 18px; }
.lobby-modal__head { display: flex; align-items: center; justify-content: space-between; }
.lobby-modal__title { display: inline-flex; align-items: center; gap: 10px; font-size: 24px; font-weight: 600; color: var(--yellow); }

.lb-list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 8px; overflow-y: auto; }
.lb-row {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 10px 16px;
  border-radius: 14px;
  background: rgba(255, 255, 255, 0.05);
}
.lb-row__rank { width: 28px; text-align: center; font-size: 18px; font-weight: 600; color: var(--text-muted); }
.lb-row__name { flex-grow: 1; font-weight: 700; font-size: 14px; }
.lb-row__elo { font-size: 17px; font-weight: 600; }
.lb-row.is-top-1 .lb-row__rank { color: #ffd21f; }
.lb-row.is-top-2 .lb-row__rank { color: #d6dcf0; }
.lb-row.is-top-3 .lb-row__rank { color: #ff9a52; }
.lb-row.is-me { background: rgba(255, 201, 74, 0.14); box-shadow: inset 0 0 0 1.5px var(--yellow); }

.rules { display: flex; flex-direction: column; gap: 14px; overflow-y: auto; padding-right: 4px; }
.rules__item h3 { margin: 0 0 4px; font-size: 17px; font-weight: 600; }
.rules__item p { margin: 0; font-size: 14px; line-height: 1.55; color: var(--text-muted); }
```

- [ ] **Step 6: Checkpoint**

Run: `npm run build` → `✓ built in ...`.
(git: `feat: add lobby top bar, dock and modals`)

---

### Task 6: Viết lại trang Sảnh

**Files:**
- Modify (thay toàn bộ): `src/pages/Lobby.jsx`
- Modify: `src/components/lobby/lobby.css` (nối thêm)

- [ ] **Step 1: Thay toàn bộ `Lobby.jsx`**

```jsx
import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import MenuBackground from '../components/MenuBackground.jsx';
import TopBar from '../components/lobby/TopBar.jsx';
import ModeCarousel from '../components/lobby/ModeCarousel.jsx';
import Dock from '../components/lobby/Dock.jsx';
import LeaderboardModal from '../components/lobby/LeaderboardModal.jsx';
import RulesModal from '../components/lobby/RulesModal.jsx';
import { IconBook, IconPlay, IconTrophy } from '../components/Icons.jsx';
import { DEFAULT_MODE, LOBBY_MODES } from '../data/lobbyModes';
import { ME } from '../data/mockData';
import '../components/lobby/lobby.css';

const START_INDEX = Math.max(0, LOBBY_MODES.findIndex((m) => m.key === DEFAULT_MODE));

export default function Lobby() {
  const navigate = useNavigate();
  const [active, setActive] = useState(START_INDEX);
  const [modal, setModal] = useState(null); // 'rank' | 'rules' | null
  const [muted, setMuted] = useState(false);
  const [toast, setToast] = useState(null);

  const notify = (text) => setToast({ id: Date.now(), text });
  const closeModal = () => setModal(null);
  const enter = (m) => navigate(m.to);
  const mode = LOBBY_MODES[active];

  return (
    // Không dùng class `lobby`: `.screen.lobby { flex-direction: row }` cũ trong pages.css
    // còn tồn tại tới Task 8.
    <div className="screen">
      <MenuBackground cards={false} />

      <TopBar
        me={ME}
        muted={muted}
        onToggleMute={() => setMuted((v) => !v)}
        onProfile={() => notify('Hồ sơ: sắp ra mắt')}
        onSettings={() => notify('Cài đặt: sắp ra mắt')}
      />

      <main className="lobby__stage">
        <ModeCarousel modes={LOBBY_MODES} active={active} onChange={setActive} onSelect={enter} keyboard={!modal} />
      </main>

      <section key={mode.key} className="lobby__info">
        <h2 className="display lobby__mode-title">{mode.title}</h2>
        <p className="lobby__mode-desc">{mode.desc}</p>
        <div className="lobby__cta">
          <span className={`chip lobby__chip ${mode.ranked ? 'is-ranked' : ''}`}>
            {mode.ranked && <IconTrophy size={13} />} {mode.chip}
          </span>
          <button type="button" className="btn btn--primary lobby__play" onClick={() => enter(mode)}>
            <IconPlay /> CHƠI NGAY
          </button>
        </div>
      </section>

      <div className="lobby__dots">
        {LOBBY_MODES.map((m, i) => (
          <button
            key={m.key}
            type="button"
            className={`lobby__dot ${i === active ? 'is-on' : ''}`}
            onClick={() => setActive(i)}
            aria-label={m.title}
          />
        ))}
      </div>

      <Dock
        items={[
          { key: 'rank', label: 'BXH', icon: IconTrophy, onClick: () => setModal('rank') },
          { key: 'rules', label: 'Luật chơi', icon: IconBook, onClick: () => setModal('rules') },
        ]}
      />
      <div className="lobby__hint">← → chọn chế độ · Enter để chơi</div>

      <LeaderboardModal open={modal === 'rank'} onClose={closeModal} />
      <RulesModal open={modal === 'rules'} onClose={closeModal} />

      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}
```

- [ ] **Step 2: Nối CSS bố cục Sảnh vào cuối `lobby.css`**

```css
/* ---------- Bố cục Sảnh ---------- */
.lobby__stage { position: relative; z-index: 2; flex-grow: 1; min-height: 0; }
.lobby__info {
  position: relative;
  z-index: 3;
  text-align: center;
  animation: info-in 0.35s ease both;
}
@keyframes info-in { from { opacity: 0; transform: translateY(8px); } }
.lobby__mode-title { margin: 0; font-size: 34px; font-weight: 700; text-shadow: 0 3px 0 rgba(0, 0, 0, 0.3); }
.lobby__mode-desc { margin: 6px auto 14px; max-width: 560px; font-size: 14px; color: #c9ccf2; }
.lobby__cta { display: flex; align-items: center; justify-content: center; gap: 16px; }
.lobby__chip { background: rgba(255, 255, 255, 0.1); color: var(--text-muted); }
.lobby__chip.is-ranked { background: rgba(255, 201, 74, 0.16); color: var(--yellow); }
.lobby__play { height: 54px; padding: 0 38px; }

.lobby__dots { position: relative; z-index: 3; display: flex; justify-content: center; gap: 8px; margin: 16px 0 18px; }
.lobby__dot {
  width: 9px;
  height: 9px;
  padding: 0;
  border: none;
  border-radius: 99px;
  background: rgba(255, 255, 255, 0.28);
  transition: width 0.25s, background 0.25s;
}
.lobby__dot.is-on { width: 26px; background: var(--yellow); }

.lobby__hint {
  position: absolute;
  right: 40px;
  bottom: 34px;
  z-index: 3;
  font-size: 12px;
  font-weight: 600;
  color: var(--text-faint);
}
```

- [ ] **Step 3: Checkpoint**

Run: `npm run build` → `✓ built in ...`.
(Lúc này `/mode` và `/room/:mode` vẫn còn nhưng không còn nút nào dẫn tới — dọn ở Task 7.)
(git: `feat: rebuild lobby as mode carousel`)

---

### Task 7: Trang Custom + route + xóa trang cũ

**Files:**
- Create: `src/pages/CustomRoom.jsx`
- Modify: `src/App.jsx`
- Delete: `src/pages/ModeSelect.jsx`, `src/pages/Room.jsx`

- [ ] **Step 1: Tạo `CustomRoom.jsx`**

```jsx
import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import Modal from '../components/Modal.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconBack, IconClose, IconCopy } from '../components/Icons.jsx';
import { BOT_LEVELS, FRIENDS, ME } from '../data/mockData';
import './pages.css';

const ROOM_CODE = 'B7K2';

// teams[i] = đội của ghế i ('A' = đội bạn). Khớp teammateSeat trong game/constants.js.
const RULES = [
  { key: 'classic', label: 'Classic', sub: 'Mỗi người một mình', teams: null },
  { key: '2v2', label: '2 vs 2', sub: 'Đồng đội ngồi đối diện', teams: ['A', 'B', 'A', 'B'] },
  { key: 'side', label: 'Side to Side', sub: 'Đồng đội ngồi cạnh', teams: ['A', 'B', 'B', 'A'] },
];
const SEAT_POS = ['bottom', 'left', 'top', 'right'];

// Ghế: null | { kind: 'me' | 'friend', person } | { kind: 'bot', level }
const newBot = () => ({ kind: 'bot', level: BOT_LEVELS[1] });
const botPerson = (i) => ({ id: `bot-${i}`, name: `Bot ${i}`, initials: '🤖', tint: '#5B6180' });

export default function CustomRoom() {
  const navigate = useNavigate();
  const [ruleKey, setRuleKey] = useState('classic');
  const [seats, setSeats] = useState([{ kind: 'me', person: ME }, null, null, null]);
  const [pickSeat, setPickSeat] = useState(null); // ghế đang chờ chọn bạn để mời
  const [joinOpen, setJoinOpen] = useState(false);
  const [joinCode, setJoinCode] = useState('');
  const [toast, setToast] = useState(null);

  const rule = RULES.find((r) => r.key === ruleKey);
  const filled = seats.filter(Boolean).length;
  const notify = (text) => setToast({ id: Date.now(), text });
  const inRoom = (f) => seats.some((s) => s?.person?.id === f.id);
  const placeAt = (i, occ) => setSeats((s) => s.map((x, k) => (k === i ? occ : x)));

  const clearSeat = (i) => {
    placeAt(i, null);
    if (pickSeat === i) setPickSeat(null);
  };

  const invite = (f) => {
    const target = pickSeat != null && !seats[pickSeat] ? pickSeat : seats.findIndex((s) => !s);
    if (target < 0) return notify('Phòng đã đủ người');
    placeAt(target, { kind: 'friend', person: f });
    setPickSeat(null);
    notify(`${f.name} đã vào phòng`);
  };

  const fillBots = () => {
    setSeats((s) => s.map((x) => x ?? newBot()));
    setPickSeat(null);
  };

  const copyCode = async () => {
    try {
      await navigator.clipboard.writeText(ROOM_CODE);
      notify('Đã sao chép mã phòng');
    } catch {
      notify(`Mã phòng: ${ROOM_CODE}`);
    }
  };

  const submitJoin = (e) => {
    e.preventDefault();
    if (joinCode.trim().length < 4) return notify('Mã phòng gồm 4 ký tự');
    setJoinOpen(false);
    notify(`Không tìm thấy phòng ${joinCode}`);
    setJoinCode('');
  };

  return (
    <div className="screen">
      <MenuBackground cards={false} />

      <header className="page-head">
        <button type="button" className="icon-btn" onClick={() => navigate('/lobby')} aria-label="Quay lại">
          <IconBack />
        </button>
        <div style={{ flexGrow: 1 }}>
          <div className="display page-head__title">Phòng tùy chỉnh</div>
          <div className="muted" style={{ fontSize: 13, marginTop: 2 }}>Mời bạn bè hoặc thêm bot · không tính ELO</div>
        </div>
        <button type="button" className="btn btn--ghost" onClick={() => setJoinOpen(true)}>Nhập mã phòng</button>
        <button type="button" className="room-code" onClick={copyCode} aria-label="Sao chép mã phòng">
          <span className="muted" style={{ fontSize: 11 }}>MÃ PHÒNG</span>
          <b className="display">{ROOM_CODE}</b>
          <IconCopy />
        </button>
      </header>

      <div className="room">
        <div className="room__table-wrap">
          <div className="rule-tabs" role="tablist" aria-label="Luật chơi">
            {RULES.map((r) => (
              <button
                key={r.key}
                type="button"
                role="tab"
                aria-selected={r.key === ruleKey}
                className={`rule-tab ${r.key === ruleKey ? 'is-on' : ''}`}
                onClick={() => setRuleKey(r.key)}
              >
                <b>{r.label}</b>
                <span>{r.sub}</span>
              </button>
            ))}
          </div>

          <div className="room-table">
            <div className="room-table__felt"><span className="display">UNO</span></div>
            {SEAT_POS.map((pos, i) => {
              const occ = seats[i];
              const team = rule.teams?.[i];
              const person = occ?.kind === 'bot' ? botPerson(i) : occ?.person ?? null;
              const rel = team ? (team === 'A' ? 'is-team' : 'is-enemy') : '';
              return (
                <div
                  key={pos}
                  className={`room-seat room-seat--${pos} ${occ ? '' : 'is-empty'} ${rel} ${pickSeat === i ? 'is-picking' : ''}`}
                >
                  {occ && occ.kind !== 'me' && (
                    <button type="button" className="seat-remove" onClick={() => clearSeat(i)} aria-label="Bỏ khỏi ghế">
                      <IconClose size={14} />
                    </button>
                  )}
                  <Avatar person={person} size={56} />
                  <div className="room-seat__name">
                    {!occ ? 'Ô trống' : occ.kind === 'me' ? `${person.name} (Bạn)` : person.name}
                  </div>
                  {occ?.kind === 'me' && <span className="chip seat-chip seat-chip--host">Chủ phòng</span>}
                  {occ && occ.kind !== 'me' && team && (
                    <span className={`chip seat-chip ${team === 'A' ? 'seat-chip--team' : 'seat-chip--enemy'}`}>
                      {team === 'A' ? 'Đồng đội' : 'Đối thủ'}
                    </span>
                  )}
                  {occ?.kind === 'bot' && (
                    <select
                      className="seat-level"
                      value={occ.level}
                      onChange={(e) => placeAt(i, { ...occ, level: e.target.value })}
                      aria-label={`Độ khó Bot ${i}`}
                    >
                      {BOT_LEVELS.map((l) => (
                        <option key={l} value={l}>{l}</option>
                      ))}
                    </select>
                  )}
                  {!occ && (
                    <div className="seat-actions">
                      <button type="button" className="btn btn--sm btn--ghost" onClick={() => setPickSeat(i)}>Mời bạn</button>
                      <button type="button" className="btn btn--sm btn--primary" onClick={() => placeAt(i, newBot())}>+ Bot</button>
                    </div>
                  )}
                </div>
              );
            })}
          </div>

          {rule.teams && (
            <div className="room__legend">
              <span><i className="dot" style={{ background: 'var(--green)' }} /> Đội của bạn</span>
              <span><i className="dot" style={{ background: 'var(--red)' }} /> Đội đối thủ</span>
            </div>
          )}
        </div>

        <div className="room__side">
          <div className={`panel room__box custom-friends ${pickSeat != null ? 'is-picking' : ''}`}>
            <div className="section-title">
              {pickSeat != null ? 'CHỌN BẠN ĐỂ MỜI VÀO GHẾ TRỐNG' : `BẠN BÈ · ${FRIENDS.filter((f) => f.online).length} ONLINE`}
            </div>
            <div className="friend-list">
              {FRIENDS.map((f) => {
                const here = inRoom(f);
                return (
                  <div key={f.id} className={`friend ${f.online ? '' : 'is-offline'}`}>
                    <div className="friend__avatar">
                      <Avatar person={f} size={40} />
                      <span className={`friend__status ${f.online ? 'is-on' : ''}`} />
                    </div>
                    <div style={{ flexGrow: 1, minWidth: 0 }}>
                      <div style={{ fontWeight: 700, fontSize: 14 }}>{f.name}</div>
                      <div className="muted friend__sub">{f.status}</div>
                    </div>
                    <button
                      type="button"
                      className={`btn btn--sm ${here ? 'btn--outline' : 'btn--ghost'}`}
                      disabled={!f.online || here}
                      onClick={() => invite(f)}
                    >
                      {here ? 'Trong phòng' : 'Mời'}
                    </button>
                  </div>
                );
              })}
            </div>
          </div>

          <div style={{ flexGrow: 1 }} />

          <button type="button" className="btn btn--outline btn--block" disabled={filled === 4} onClick={fillBots}>
            🤖 Thêm bot vào mọi ô trống
          </button>
          <button
            type="button"
            className="btn btn--primary btn--block"
            disabled={filled < 4}
            onClick={() => navigate(`/game/${ruleKey}`)}
            title={filled < 4 ? 'Cần đủ 4 ghế' : undefined}
          >
            {filled < 4 ? `Bắt đầu (${filled}/4)` : 'Bắt đầu'}
          </button>
        </div>
      </div>

      <Modal open={joinOpen} onClose={() => setJoinOpen(false)} label="Nhập mã phòng">
        <form className="join-form" onSubmit={submitJoin}>
          <div className="display" style={{ fontSize: 22, fontWeight: 600 }}>Nhập mã phòng</div>
          <input
            className="input join-form__input"
            placeholder="VD: B7K2"
            maxLength={4}
            autoFocus
            value={joinCode}
            onChange={(e) => setJoinCode(e.target.value.toUpperCase())}
            aria-label="Mã phòng"
          />
          <div style={{ display: 'flex', gap: 12 }}>
            <button type="button" className="btn btn--outline" style={{ flex: 1 }} onClick={() => setJoinOpen(false)}>Huỷ</button>
            <button type="submit" className="btn btn--primary" style={{ flex: 1 }}>Vào phòng</button>
          </div>
        </form>
      </Modal>

      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}
```

- [ ] **Step 2: Sửa `App.jsx`**

Thay toàn bộ file bằng:
```jsx
import { Navigate, Route, Routes } from 'react-router-dom';
import Stage from './components/Stage.jsx';
import Login from './pages/Login.jsx';
import Lobby from './pages/Lobby.jsx';
import CustomRoom from './pages/CustomRoom.jsx';
import Matchmaking from './pages/Matchmaking.jsx';
import GameTable from './pages/GameTable.jsx';

export default function App() {
  return (
    <Stage>
      <Routes>
        <Route path="/" element={<Login />} />
        <Route path="/lobby" element={<Lobby />} />
        <Route path="/custom" element={<CustomRoom />} />
        <Route path="/matchmaking/:mode" element={<Matchmaking />} />
        <Route path="/game/:mode" element={<GameTable />} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    </Stage>
  );
}
```

- [ ] **Step 3: Xóa trang cũ**

Run: `rm src/pages/ModeSelect.jsx src/pages/Room.jsx`

- [ ] **Step 4: Kiểm tra không còn tham chiếu**

Run: `grep -rn "ModeSelect\|pages/Room\|'/mode'\|/room/" src`
Expected: không có kết quả.

- [ ] **Step 5: Checkpoint**

Run: `npm run build` → `✓ built in ...`.
(git: `feat: add custom room, remove mode select and old room`)

---

### Task 8: Dọn `pages.css` + CSS phòng Custom

**Files:**
- Modify: `src/pages/pages.css`

- [ ] **Step 1: Xóa CSS sidebar/lobby/mode-select cũ, giữ `friend*`**

Chạy script (xóa từ dòng `/* ---------- Lobby ---------- */` tới trước `/* ---------- Room ---------- */`, rồi chèn lại khối `friend*` cần cho CustomRoom):

```bash
node -e '
const fs = require("fs");
const p = "src/pages/pages.css";
let s = fs.readFileSync(p, "utf8");
const a = s.indexOf("/* ---------- Lobby ---------- */");
const b = s.indexOf("/* ---------- Room ---------- */");
if (a < 0 || b < 0 || b < a) throw new Error("markers not found");
const fStart = s.indexOf(".friend-list {", a);
const fEnd = s.indexOf("/* ---------- Mode select ---------- */", a);
const friends = s.slice(fStart, fEnd).trimEnd();
s = s.slice(0, a) + "/* ---------- Friends (dùng trong Custom) ---------- */\n" + friends + "\n\n" + s.slice(b);
fs.writeFileSync(p, s);
'
```

- [ ] **Step 2: Xóa `ready-bars` và `room__ready` (không còn dùng)**

Xóa 4 dòng sau trong `pages.css`:
```css
.room__ready { flex-direction: row; align-items: center; justify-content: space-between; }
.ready-bars { display: flex; gap: 6px; }
.ready-bars span { width: 30px; height: 7px; border-radius: 999px; background: var(--line); transition: background 0.3s, box-shadow 0.3s; }
.ready-bars span.is-on { background: var(--green); box-shadow: 0 0 10px rgba(53, 211, 153, 0.6); }
```

- [ ] **Step 3: Thêm CSS Custom vào cuối khối Room (ngay trước `/* ---------- Matchmaking ---------- */`)**

```css
/* ---------- Custom room ---------- */
.rule-tabs { display: flex; gap: 10px; }
.rule-tab {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 2px;
  padding: 10px 18px;
  border-radius: 14px;
  border: 2px solid transparent;
  background: var(--bg-elevated);
  text-align: left;
  transition: border-color 0.2s, background 0.2s;
}
.rule-tab b { font-size: 14px; }
.rule-tab span { font-size: 11px; color: var(--text-muted); }
.rule-tab:hover { background: var(--bg-elevated-2); }
.rule-tab.is-on { border-color: var(--yellow); background: rgba(255, 201, 74, 0.12); }
.rule-tab.is-on b { color: var(--yellow); }

.room-seat.is-picking { border-color: var(--yellow); border-style: solid; box-shadow: 0 0 0 4px rgba(255, 201, 74, 0.2), 0 14px 30px rgba(0, 0, 0, 0.35); }
.seat-chip { background: rgba(255, 255, 255, 0.08); }
.seat-chip--host { color: var(--yellow); background: rgba(255, 201, 74, 0.12); }
.seat-chip--team { color: var(--green); background: rgba(53, 211, 153, 0.12); }
.seat-chip--enemy { color: var(--red); background: rgba(255, 82, 98, 0.12); }
.seat-actions { display: flex; gap: 6px; }
.seat-level {
  height: 30px;
  padding: 0 10px;
  border-radius: 10px;
  border: 1px solid var(--line);
  background: var(--bg-soft);
  color: var(--text);
  font: inherit;
  font-size: 12px;
  font-weight: 700;
}
.seat-remove {
  position: absolute;
  top: 8px;
  right: 8px;
  width: 26px;
  height: 26px;
  border: none;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(255, 255, 255, 0.08);
  color: var(--text-muted);
}
.seat-remove:hover { background: rgba(255, 82, 98, 0.2); color: var(--red); }

.custom-friends { transition: border-color 0.2s, box-shadow 0.2s; }
.custom-friends.is-picking { border-color: var(--yellow); box-shadow: 0 0 0 4px rgba(255, 201, 74, 0.15); }
.join-form { display: flex; flex-direction: column; gap: 18px; width: 340px; }
.join-form__input { text-align: center; font-size: 24px; font-weight: 800; letter-spacing: 0.3em; }
```

- [ ] **Step 4: Kiểm tra class cũ đã hết**

Run: `grep -n "sidebar\|play-hero\|featured\|mode-card\|quick-rank\|lobby__\|code-box\|ready-bars" src/pages/pages.css`
Expected: không có kết quả.

- [ ] **Step 5: Checkpoint**

Run: `npm run build` → `✓ built in ...`.
(git: `style: remove old lobby css, add custom room css`)

---

### Task 9: Tài liệu

**Files:**
- Modify: `IMAGE_SPECIFICATIONS.md` (thêm mục 3.6 trước `## 4.`, thêm cây thư mục ở mục 4, thêm prompt ở mục 5)
- Modify: `README.md` (client, dòng 12–14)
- Modify: `../README.md` (repo gốc, mục "Trạng thái hiện tại")

- [ ] **Step 1: `IMAGE_SPECIFICATIONS.md` — thêm mục 3.6 ngay trước dòng `## 4. Bảng tổng hợp ...`**

```markdown
### 3.6. Thẻ chế độ chơi ở Sảnh (Mode Cards)
Thư mục lưu trữ: `client/public/images/modes/`
Khai báo trong: `client/src/data/lobbyModes.js` (trường `image`)

- **Kích thước:** `600×900 px` (tỉ lệ 2:3), PNG 32-bit, nền trong suốt ngoài 4 góc bo.
- **Thiết kế:** ảnh tự chứa toàn bộ lá bài — viền trắng dày, bo góc ~36px, tên chế độ chữ to đậm có viền đen. Code chỉ hiển thị ảnh, không vẽ thêm gì lên trên.
- **Thiếu ảnh:** Sảnh hiện khung màu trơn kèm tên chế độ, vẫn dùng được.

| File | Chế độ | Màu chủ đạo | Ý tưởng hình |
|---|---|---|---|
| `classic.png` | Classic | Đỏ `#E8212E` | Lá UNO đỏ kinh điển, oval trắng nghiêng, chữ "UNO CLASSIC" vàng |
| `2v2.png` | 2 vs 2 | Xanh lá `#1FA64A` | Hai cặp tay cầm bài đối diện nhau, chữ "2 VS 2" |
| `side.png` | Side to Side | Tím `#7B3FE4` | Hai người ngồi cạnh nhau cùng giơ bài, chữ "SIDE TO SIDE" |
| `custom.png` | Custom | Vàng cam `#F0A800` | Ngôi nhà/bàn chơi riêng có robot bot và bạn bè, chữ "CUSTOM" |
```

- [ ] **Step 2: `IMAGE_SPECIFICATIONS.md` — mục 4, thêm vào cây thư mục ngay sau khối `├── ui/ ...` (trước `└── avatars/`)**

```
├── modes/
│   ├── classic.png        # Thẻ chế độ Classic (600x900)
│   ├── 2v2.png            # Thẻ chế độ 2 vs 2
│   ├── side.png           # Thẻ chế độ Side to Side
│   └── custom.png         # Thẻ chế độ Custom
```
và đổi tiêu đề mục 4 từ `61 File` thành `65 File`.

- [ ] **Step 3: `IMAGE_SPECIFICATIONS.md` — mục 5, thêm ngay trước `## 6.`**

````markdown
### Prompt cho Thẻ chế độ (`modes/*.png`):
Đổi phần trong ngoặc vuông cho từng chế độ (xem bảng mục 3.6):
```text
Vertical playing-card shaped game mode tile, 2:3 aspect ratio, thick white rounded border, vibrant [RED] glossy background, [a classic UNO card with a tilted white oval] illustration in the center, bold chunky tilted cartoon title text "[UNO CLASSIC]" in yellow with thick black outline, 3D casual mobile game UI style, soft glow, sparkles, high detail, isolated on transparent background --ar 2:3 --no background
```
````

- [ ] **Step 4: `client/README.md` — thay đoạn luồng màn hình (dòng 12–14)**

```markdown
Luồng màn hình: `/` Đăng nhập → `/lobby` Sảnh (carousel chọn chế độ) →
Classic / 2 vs 2 / Side to Side: `/matchmaking/:mode` Ghép trận → `/game/:mode` Bàn chơi (tính ELO);
Custom: `/custom` Phòng tùy chỉnh (mời bạn / thêm bot) → `/game/:mode` (không tính ELO).
`:mode` = `classic` | `2v2` | `side`. Thêm chế độ lên carousel: sửa `src/data/lobbyModes.js`.
```

- [ ] **Step 5: `../README.md` (gốc) — mục "Trạng thái hiện tại", thay dòng luồng màn hình**

Thay:
```markdown
- Luồng màn hình: Đăng nhập → Sảnh → Chọn chế độ → Phòng chờ (2vs2 / Side) →
  Ghép trận → Bàn chơi.
```
bằng:
```markdown
- Luồng màn hình: Đăng nhập → Sảnh (carousel thẻ chế độ) → Ghép trận (Classic /
  2 vs 2 / Side to Side) hoặc Phòng Custom (mời bạn, thêm bot) → Bàn chơi.
- ELO, bảng xếp hạng, độ khó bot hiện chỉ là giao diện với dữ liệu giả.
```

- [ ] **Step 6: Checkpoint**

(git: `docs: document lobby carousel and mode card images`)

---

### Task 10: Kiểm tra trên trình duyệt

**Files:** không sửa (chỉ sửa nếu phát hiện lỗi).

- [ ] **Step 1: Build sạch**

Run: `npm run build`
Expected: `✓ built in ...`, không lỗi.

- [ ] **Step 2: Chạy dev server**

Run (nền): `npm run dev` → mở `http://localhost:5173/lobby`.

- [ ] **Step 3: Kiểm Sảnh**

Đối chiếu với mockup `full-screen-v2.html` (`.superpowers/brainstorm/.../content/`):
- Thẻ Classic ở giữa, viền vàng; 2 vs 2 bên trái, Side to Side bên phải; Custom ẩn (4 thẻ → chỉ ±1).
- Chưa có ảnh → cả 4 thẻ hiện khung màu có tên chế độ.
- Phím → / ←: đổi thẻ, tên/mô tả/chip bên dưới đổi theo, chấm vị trí đổi.
- Cuộn chuột trên carousel: đổi đúng 1 thẻ mỗi nấc.
- Bấm thẻ bên → xoay vào giữa; bấm thẻ giữa hoặc Enter hoặc CHƠI NGAY → Classic/2v2/Side vào `/matchmaking/<key>`, Custom vào `/custom`.
- Chip: 3 chế độ đầu "XẾP HẠNG · ELO" màu vàng có cúp; Custom "KHÔNG TÍNH ELO".
- BXH mở popup, dòng "Bách (Bạn)" tô sáng; Esc hoặc ✕ đóng. Khi popup mở, ← → không đổi thẻ.
- Luật chơi mở popup, cuộn được.
- Bấm thẻ hồ sơ / nút cài đặt → toast "sắp ra mắt"; nút âm thanh đổi icon.

- [ ] **Step 4: Kiểm Custom (`/custom`)**

- Tab Classic: không có chip đội, không có chú thích đội. Tab 2 vs 2: ghế trên xanh (đồng đội), trái/phải đỏ. Tab Side: ghế phải xanh.
- "+ Bot" trên ghế trống → ghế thành "Bot n" có dropdown Dễ/Thường/Khó, đổi được; ✕ bỏ bot.
- "Mời bạn" trên ghế → ghế viền vàng, panel bạn bè viền vàng + tiêu đề "CHỌN BẠN..."; bấm Mời 1 bạn → vào đúng ghế đó, nút đổi "Trong phòng".
- Bạn offline không mời được.
- "Thêm bot vào mọi ô trống" → đủ 4, nút Bắt đầu bật → vào `/game/<rule>` và chơi được.
- "Nhập mã phòng" → popup; mã < 4 ký tự → toast lỗi; đủ 4 → toast "Không tìm thấy phòng ...".
- Nút quay lại → `/lobby`.

- [ ] **Step 5: Kiểm luồng xếp hạng**

`/lobby` → Classic → Ghép trận hiện "Classic" → tự vào `/game/classic`, bàn chơi 4 người không chia đội. "Huỷ tìm trận" và nút quay lại → `/lobby`. Lặp lại với 2 vs 2 và Side to Side. Hết trận → "Về sảnh" về carousel.

- [ ] **Step 6: Kiểm với ảnh thật (tùy chọn)**

Đặt tạm 1 PNG bất kỳ vào `public/images/modes/classic.png` → thẻ Classic hiện ảnh, 3 thẻ còn lại vẫn placeholder. Xóa ảnh tạm sau khi kiểm.
