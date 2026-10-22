# QUY CHUẨN THIẾT KẾ & ĐẶC TẢ HÌNH ẢNH (ASSETS SPECIFICATION)
> **Dự án:** UNO Online Multiplayer (Client ReactJS + Vite)  
> **Thư mục chứa ảnh:** `client/public/images/`  
> **File cấu hình nạp ảnh:** `client/src/config/assets.js`

---

## 1. Định hướng phong cách nghệ thuật (Art Direction)

- **Phong cách chủ đạo:** 3D Casual / Arcade rực rỡ, hiện đại, vui tươi và tràn đầy năng lượng (tương tự phong cách đồ hoạ của *UNO Mobile*, *Clash Royale*, *Brawl Stars*).
- **Màu sắc nhận diện (Brand Colors):**
  - Đỏ (Red): `#E3301C` (Màu cờ UNO, rực cháy, tương phản cao)
  - Vàng (Yellow): `#F8B600` / `#FFC94A` (Ấm áp, vui tươi)
  - Xanh lá (Green): `#12B45B` / `#35D399` (Tươi mát, hiện đại)
  - Xanh dương (Blue): `#1E75EB` / `#4C86FF` (Sâu lắng, bắt mắt)
  - Đen/Wild (Wild): Nền đen sang trọng `#1E1E28` kết hợp vòng chuyển màu 4 sắc cầu vồng
- **Hiệu ứng thị giác:**
  - Viền bo cong mềm mại, độ nổi khối 3D rõ ràng (bevel/emboss nhẹ, ambient occlusion).
  - Ánh sáng phản chiếu bóng bẩy (glossy highlight) trên bề mặt bài và nút bấm.
  - Tối ưu hiển thị rõ ràng trên cả màn hình máy tính và thiết bị di động (High-DPI / Retina).

---

## 2. Quy chuẩn kỹ thuật chung

| Tiêu chí | Quy định | Ghi chú |
|---|---|---|
| **Hệ màu** | sRGB | Tránh sai lệch màu giữa các trình duyệt |
| **Độ phân giải hiển thị** | Tối thiểu @2x | Đảm bảo hình ảnh sắc nét, không vỡ hạt trên màn hình HD/4K |
| **Định dạng bài & UI** | **PNG 32-bit (với kênh Alpha trong suốt)** | Giữ viền bo cong sạch sẽ, không bị viền trắng hay răng cưa |
| **Định dạng nền (Background)** | **JPG** (chất lượng 85–92%) hoặc **WebP** | Tối ưu dung lượng tải trang (< 350KB/ảnh nền) |
| **Đặt tên file** | `lowercase`, nối từ bằng dấu gạch dưới `_` hoặc gạch ngang `-` | Khớp chính xác với cấu hình code trong `assets.js` |

---

## 3. Chi tiết đặc tả từng loại hình ảnh

### 3.1. Hình nền (Backgrounds)
Thư mục lưu trữ: `client/public/images/backgrounds/`

#### ① Nền bàn chơi chính (`table.jpg`)
- **Khoá trong ASSETS:** `backgrounds.table`
- **Kích thước khuyến nghị:** `1920×1080` hoặc `2560×1600` (Tỷ lệ 16:9 hoặc 16:10, tối thiểu `1440×900`)
- **Định dạng:** JPG chất lượng cao (hoặc WebP)
- **Mô tả chi tiết:**
  - **Bối cảnh:** Góc nhìn từ trên xuống nghiêng nhẹ (top-down view) tựa như bàn chơi gameshow casino/arcade sống động.
  - **Màu nền:** Chuyển sắc từ đỏ tươi ở trung tâm sang đỏ mận sẫm / hạt dẻ ở các góc (Radial Gradient).
  - **Chiếu sáng:** Hai luồng đèn sân khấu (spotlight) chiếu xéo từ hai góc trên vào trung tâm bàn; có vệt sáng hoặc tia sáng xoay nhẹ mờ ảo.
  - **Chi tiết ẩn:** Chữ "UNO" mờ chìm (watermark) khổ lớn ở chính giữa bàn, độ mờ (opacity) khoảng 8–12% để không làm rối mắt.
  - **Lưu ý UI:** Khu vực trung tâm (nơi đặt bộ bài rút và chồng bài đánh) và dải dưới (bài trên tay người chơi) cần có độ tối và tương phản hợp lý để các lá bài nổi bật rõ ràng.

#### ② Nền các màn hình Menu & Sảnh (`menu.jpg`)
- **Khoá trong ASSETS:** `backgrounds.menu`
- **Kích thước khuyến nghị:** `1920×1080` (Tỷ lệ 16:9, tối thiểu `1440×900`)
- **Định dạng:** JPG hoặc WebP
- **Mô tả chi tiết:**
  - **Bối cảnh:** Không gian gaming/lounge tương lai, sang trọng và dịu mắt.
  - **Màu sắc:** Tông xanh tím than chủ đạo (Deep Indigo / Space Blue `#0E122A` đến `#181B38`), kết hợp các quầng sáng lan tỏa (neon orbs) màu tím lavender, xanh dương và cam hồng.
  - **Họa tiết:** Lưới không gian (grid lines) mờ ảo phía chân trời, có chiều sâu tạo cảm giác hiện đại.
  - **Lưu ý UI:** Nền menu dùng chung cho trang Sảnh (Lobby), Chọn chế độ (Mode Select), Phòng chờ (Room) và Ghép trận (Matchmaking). Tránh các hình khối quá rực rỡ ở chính giữa vì sẽ bị che bởi các bảng danh sách và pop-up.

#### ③ Nền cột trái màn hình Đăng nhập (`login.jpg`)
- **Khoá trong ASSETS:** `backgrounds.login`
- **Kích thước khuyến nghị:** `800×1200` (Hiển thị thực tế: `640×900`, tỷ lệ dọc)
- **Định dạng:** JPG hoặc WebP
- **Mô tả chi tiết:**
  - **Bối cảnh:** Nền banner dọc mang phong cách lễ hội bùng nổ năng lượng của UNO.
  - **Màu sắc:** Chuyển sắc đỏ cam rực rỡ (`#FF7D22` trung tâm tỏa ra `#E3301C` và viền đỏ sẫm `#5E0610`).
  - **Họa tiết:** Hào quang tỏa tia (sunburst rays) nhẹ nhàng, các đốm hạt ánh sáng lấp lánh (light particles / bokeh).
  - **Lưu ý bố cục:** Phần trên cùng (1/3 khung hình) để khoảng thở cho các lá bài bay xòe quạt; phần giữa cho Logo game; phần chân đáy cho các thông số thống kê người chơi.

---

### 3.2. Bộ lá bài UNO (UNO Cards)
Thư mục lưu trữ: `client/public/images/cards/`  
**Kích thước chuẩn:** `480×720 px` (Tỷ lệ chuẩn 2:3, tối thiểu `240×360 px`)  
**Định dạng:** PNG 32-bit (Nền trong suốt ngoài 4 góc bo của lá bài)

#### ① Mặt sau lá bài (`back.png`)
- **Khoá trong ASSETS:** `cardBack`
- **Đường dẫn:** `images/cards/back.png`
- **Mô tả chi tiết:**
  - **Viền ngoài:** Viền trắng dày đặc trưng bo cong 4 góc (`border-radius ~ 11%`).
  - **Nền lá bài:** Màu đen nhám hoặc xanh đen than `#141414`.
  - **Hình bầu dục trung tâm (Oval):** Một hình oval màu đỏ tươi viền ánh sáng vàng/trắng, nằm chéo nghiêng góc khoảng -24 đến -28 độ.
  - **Chữ trung tâm:** Logo chữ "**UNO**" cách điệu màu vàng nổi bật, đổ bóng 3D dày dặn, viền đen sắc sảo nằm lọt trong hình oval đỏ.

#### ② Quy chuẩn thiết kế Mặt trước 54 lá bài (`{color}_{value}.png`)
- **Khoá trong ASSETS:** `cardFace: (card) => \`/images/cards/${card.color}_${card.value}.png\``
- **Cấu trúc chung của 1 lá bài:**
  1. **Khung viền trắng:** Bo tròn 4 góc (độ cong mượt mà, viền trắng dày khoảng 5%–6% chiều rộng).
  2. **Vùng nền màu:** Màu đặc trưng của lá bài (`red`, `yellow`, `green`, `blue`).
  3. **Hình bầu dục nghiêng trắng (White Oval):** Nằm giữa lá bài, nghiêng chéo góc 28°, tạo độ sâu cho ký hiệu chính.
  4. **Ký hiệu trung tâm:** Hiển thị số hoặc biểu tượng chức năng to rõ, font số dày dặn (Display Sans-serif bo tròn viền), có đổ bóng nhẹ. Riêng số `6` và số `9` **bắt buộc có gạch chân** (`6_`, `9_`) để người chơi không bị đọc nhầm khi quay bài ngược.
  5. **Ký hiệu góc (Index / Pip):** Hai góc chéo (góc trên bên trái và góc dưới bên phải xoay 180°), gồm ký hiệu nhỏ giúp nhận diện khi xòe bài trên tay.

---

### 3.3. Danh sách chi tiết toàn bộ 54 lá bài mặt trước

#### Nhóm 1: 4 Màu cơ bản (13 lá/màu × 4 màu = 52 lá)

##### Lá màu ĐỎ (Red) — `images/cards/red_*.png`
| Tên file | Ký hiệu trung tâm | Ký hiệu 2 góc | Ý nghĩa / Ghi chú |
|---|---|---|---|
| `red_0.png` | Số 0 màu đỏ viền đen | Số 0 | Số 0 màu đỏ |
| `red_1.png` | Số 1 màu đỏ viền đen | Số 1 | Số 1 màu đỏ |
| `red_2.png` | Số 2 màu đỏ viền đen | Số 2 | Số 2 màu đỏ |
| `red_3.png` | Số 3 màu đỏ viền đen | Số 3 | Số 3 màu đỏ |
| `red_4.png` | Số 4 màu đỏ viền đen | Số 4 | Số 4 màu đỏ |
| `red_5.png` | Số 5 màu đỏ viền đen | Số 5 | Số 5 màu đỏ |
| `red_6.png` | Số 6 có gạch chân viền đen | Số 6 gạch chân | Số 6 màu đỏ |
| `red_7.png` | Số 7 màu đỏ viền đen | Số 7 | Số 7 màu đỏ |
| `red_8.png` | Số 8 màu đỏ viền đen | Số 8 | Số 8 màu đỏ |
| `red_9.png` | Số 9 có gạch chân viền đen | Số 9 gạch chân | Số 9 màu đỏ |
| `red_skip.png` | Biểu tượng cấm ⊘ (vòng tròn gạch chéo đỏ) | Biểu tượng cấm nhỏ | Bỏ lượt người kế tiếp |
| `red_reverse.png` | Hai mũi tên uốn cong đảo chiều nhau | Hai mũi tên nhỏ | Đảo chiều vòng chơi |
| `red_draw2.png` | Ký hiệu `+2` kèm 2 lá bài xếp lồng nhau | Ký hiệu `+2` | Người tiếp theo rút 2 lá và mất lượt |

##### Lá màu VÀNG (Yellow) — `images/cards/yellow_*.png`
| Tên file | Ký hiệu trung tâm | Ký hiệu 2 góc | Ý nghĩa / Ghi chú |
|---|---|---|---|
| `yellow_0.png` đến `yellow_9.png` | Chữ số 0 đến 9 (màu vàng viền đen, 6 và 9 có gạch chân) | Số tương ứng | 10 lá số màu vàng |
| `yellow_skip.png` | Biểu tượng cấm ⊘ màu vàng | Biểu tượng cấm nhỏ | Bỏ lượt màu vàng |
| `yellow_reverse.png` | Hai mũi tên đảo chiều màu vàng | Hai mũi tên nhỏ | Đảo chiều màu vàng |
| `yellow_draw2.png` | Ký hiệu `+2` màu vàng | Ký hiệu `+2` | Rút 2 lá màu vàng |

##### Lá màu XANH LÁ (Green) — `images/cards/green_*.png`
| Tên file | Ký hiệu trung tâm | Ký hiệu 2 góc | Ý nghĩa / Ghi chú |
|---|---|---|---|
| `green_0.png` đến `green_9.png` | Chữ số 0 đến 9 (màu xanh lá viền đen, 6 và 9 có gạch chân) | Số tương ứng | 10 lá số màu xanh lá |
| `green_skip.png` | Biểu tượng cấm ⊘ màu xanh lá | Biểu tượng cấm nhỏ | Bỏ lượt màu xanh lá |
| `green_reverse.png` | Hai mũi tên đảo chiều màu xanh lá | Hai mũi tên nhỏ | Đảo chiều màu xanh lá |
| `green_draw2.png` | Ký hiệu `+2` màu xanh lá | Ký hiệu `+2` | Rút 2 lá màu xanh lá |

##### Lá màu XANH DƯƠNG (Blue) — `images/cards/blue_*.png`
| Tên file | Ký hiệu trung tâm | Ký hiệu 2 góc | Ý nghĩa / Ghi chú |
|---|---|---|---|
| `blue_0.png` đến `blue_9.png` | Chữ số 0 đến 9 (màu xanh dương viền đen, 6 và 9 có gạch chân) | Số tương ứng | 10 lá số màu xanh dương |
| `blue_skip.png` | Biểu tượng cấm ⊘ màu xanh dương | Biểu tượng cấm nhỏ | Bỏ lượt màu xanh dương |
| `blue_reverse.png` | Hai mũi tên đảo chiều màu xanh dương | Hai mũi tên nhỏ | Đảo chiều màu xanh dương |
| `blue_draw2.png` | Ký hiệu `+2` màu xanh dương | Ký hiệu `+2` | Rút 2 lá màu xanh dương |

---

#### Nhóm 2: Lá bài Đen / Đổi màu đặc biệt (Wild Cards — 2 lá)
Thư mục lưu trữ: `client/public/images/cards/`

##### ① Lá đổi màu tự do (`wild_wild.png`)
- **Khoá sinh ra từ cardFace:** `card.color = 'wild'`, `card.value = 'wild'`
- **Nền lá bài:** Màu đen than sang trọng (`#1A1A1A`) có vân chìm tinh xảo.
- **Oval trung tâm:** Chia thành 4 phần tư hình nan quạt (Pie chart/Conic gradient) mang 4 màu chủ đạo: Đỏ, Xanh dương, Vàng, Xanh lá viền sáng bao quanh.
- **Ký hiệu góc:** Ký hiệu oval 4 màu thu nhỏ ở 2 góc.
- **Tác dụng:** Đổi màu chơi sang 1 trong 4 màu bất kỳ.

##### ② Lá đổi màu + Rút 4 lá (`wild_wild4.png`)
- **Khoá sinh ra từ cardFace:** `card.color = 'wild'`, `card.value = 'wild4'`
- **Nền lá bài:** Màu đen than sang trọng (`#1A1A1A`).
- **Hình ảnh trung tâm:** Chữ `+4` in nổi 3D màu trắng viền đen to rõ, kèm theo 4 lá bài nhỏ xòe quạt mang 4 màu Đỏ - Xanh dương - Vàng - Xanh lá.
- **Ký hiệu góc:** Ký hiệu `+4` màu trắng ở 2 góc.
- **Tác dụng:** Đổi màu chơi và buộc người kế tiếp rút 4 lá bài đồng thời mất lượt.

---

### 3.4. Giao diện & Nút bấm (UI Assets)
Thư mục lưu trữ: `client/public/images/ui/`

#### ① Logo trò chơi (`logo.png`)
- **Khoá trong ASSETS:** `logo`
- **Kích thước khuyến nghị:** `600×240 px` hoặc `800×320 px` (Hiển thị thực tế: `~400×160 px`)
- **Định dạng:** PNG 32-bit (Trong suốt hoàn toàn phần nền)
- **Mô tả chi tiết:**
  - Chữ "**UNO**" nghiêng 3D phong cách arcade bùng nổ.
  - Thân chữ màu đỏ tươi rực rỡ, viền bao quanh màu vàng chanh dày dặn và sắc nét, lớp viền ngoài cùng màu đen tạo độ tương phản mạnh.
  - Hiệu ứng ánh sáng bóng loáng (specular gloss / glint) phản chiếu trên bề mặt chữ cái, tạo cảm giác cao cấp và bắt mắt.
  - Thêm một quầng sáng mềm mại bao quanh chữ để khi đặt trên nền tối hay nền sáng đều không bị chìm.

#### ② Nút bấm HÔ UNO (`uno-button.png`)
- **Khoá trong ASSETS:** `unoButton`
- **Kích thước khuyến nghị:** `320×320 px` hoặc `512×512 px` (Hình vuông 1:1, hiển thị trong nút tròn đường kính 96px)
- **Định dạng:** PNG 32-bit (Trong suốt hoàn toàn quanh viền tròn)
- **Mô tả chi tiết:**
  - **Hình dáng:** Khối nút tròn vồng 3D (Dome Button) giống như nút bấm chuông trong các gameshow truyền hình.
  - **Vành nút:** Vành ngoài mạ chrome kim loại hoặc viền vàng đồng sáng bóng có đổ bóng chân thực.
  - **Thân nút:** Nhựa acrylic cao cấp màu đỏ tươi, bề mặt vòm cong có phản quang ánh sáng trắng chéo góc.
  - **Chữ trên nút:** Chữ "**UNO**" (hoặc dòng trên chữ nhỏ "**CALL**", dòng dưới chữ to "**UNO**") dập nổi màu vàng viền trắng ở giữa nút.
  - **Lưu ý:** Nút có trạng thái thường (bình thường) và khi active sẽ được hệ thống CSS tạo hiệu ứng nhấp nháy/phóng to thu nhỏ. Ảnh cần thiết kế ở trạng thái đầy đủ ánh sáng đẹp nhất.

---

### 3.5. Ảnh đại diện người chơi (Avatars)
Thư mục lưu trữ: `client/public/images/avatars/`  
**Kích thước chuẩn:** `256×256 px` hoặc `512×512 px` (Tỷ lệ 1:1 vuông)  
**Định dạng:** PNG hoặc JPG chất lượng cao  
**Phong cách:** Nhân vật 3D hoạt hình / Chibi hiện đại, khuôn mặt biểu cảm vui tươi, màu nền nổi bật tương ứng với profile trong game.

| Tên file gợi ý | Người chơi tương ứng | ID trong code | Màu nhận diện | Mô tả nhân vật |
|---|---|---|---|---|
| `me.png` | Người chơi chính (Bách) | `p-me` | Xanh dương (`#4C86FF`) | Nam thanh niên năng động, nụ cười tự tin, áo hoodie xanh hoặc tai nghe gaming. |
| `minh-anh.png` | Bot Minh Anh | `p-ma` | Đỏ hồng (`#FF5262`) | Bạn nữ tóc ngắn cá tính, đeo băng đô hoặc kẹp tóc dễ thương, biểu cảm tươi tắn. |
| `hoang-khang.png` | Bot Hoàng Khang | `p-hk` | Tím Indigo (`#B073FF`) | Bạn nam phong cách cool ngầu, tóc undercut hoặc đeo kính râm thể thao. |
| `thao-nguyen.png` | Bot Thảo Nguyên | `p-tn` | Xanh ngọc lục bảo (`#35D399`) | Bạn nữ tóc dài buộc đuôi ngựa, đeo kính tròn tri thức, nụ cười thân thiện. |
| `quoc-long.png` | Bạn Quốc Long | `p-ql` | Vàng cam (`#FFC94A`) | Bạn nam thông minh, vui tính, tóc xoăn nhẹ, biểu cảm hài hước. |

---

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

---

## 4. Bảng tổng hợp toàn bộ 69 File ảnh cần thiết

```
client/public/images/
├── backgrounds/
│   ├── table.jpg          # Nền bàn chơi chính (1920x1080)
│   ├── menu.jpg           # Nền sảnh, menu, phòng chờ (1920x1080)
│   └── login.jpg          # Nền cột trái đăng nhập (800x1200)
├── cards/
│   ├── back.png           # Mặt sau lá bài (480x720)
│   ├── red_0.png .. red_9.png, red_skip.png, red_reverse.png, red_draw2.png
│   ├── yellow_0.png .. yellow_9.png, yellow_skip.png, yellow_reverse.png, yellow_draw2.png
│   ├── green_0.png .. green_9.png, green_skip.png, green_reverse.png, green_draw2.png
│   ├── blue_0.png .. blue_9.png, blue_skip.png, blue_reverse.png, blue_draw2.png
│   ├── wild_wild.png      # Lá đổi màu (Wild)
│   └── wild_wild4.png     # Lá đổi màu +4 (Wild Draw 4)
├── ui/
│   ├── logo.png           # Logo UNO 3D trong suốt (~600x240)
│   └── uno-button.png     # Nút tròn 3D CALL UNO (~400x400)
├── modes/
│   ├── classic.png        # Thẻ chế độ Classic (600x900)
│   ├── 2v2.png             # Thẻ chế độ 2 vs 2
│   ├── side.png            # Thẻ chế độ Side to Side
│   └── custom.png          # Thẻ chế độ Custom
└── avatars/
    ├── me.png             # Avatar người chơi chính (256x256)
    ├── minh-anh.png       # Avatar bot Minh Anh
    ├── hoang-khang.png    # Avatar bot Hoàng Khang
    ├── thao-nguyen.png    # Avatar bot Thảo Nguyên
    └── quoc-long.png      # Avatar bạn bè Quốc Long
```

---

## 5. Hướng dẫn Prompt gợi ý khi dùng AI tạo ảnh (Midjourney / DALL-E 3)

Nếu bạn sử dụng AI để tạo ảnh, dưới đây là các câu Prompt chuẩn hoá:

### Prompt cho Nền bàn chơi (`table.jpg`):
```text
Top-down view of a vibrant red luxury board game table, smooth crimson red felt surface with a subtle watermark text "UNO" in the center, dual dynamic warm stage spotlights beams shining from the sides, soft radial vignette edges, 3D casual game style, clean, uncluttered center, cinematic lighting, 8k resolution, photorealistic rendering --ar 16:10 --stylize 250
```

### Prompt cho Nền Menu (`menu.jpg`):
```text
Modern arcade game menu background, deep indigo and navy blue abstract cyberspace, glowing neon orbs in purple and cyan, soft ambient depth of field, minimalist geometric grid lines at bottom, high-end casual game UI background, clean and sleek --ar 16:9 --stylize 200
```

### Prompt cho Logo game (`logo.png`):
```text
Vibrant 3D casual game logo with the word "UNO", bold tilted playful cartoon letters, vivid red body, thick polished golden yellow bevel borders, glossy reflections, sparkles, transparent background, vector cutout, high contrast, mobile game style, isolated on pure white background --no background
```

### Prompt cho Nút bấm HÔ UNO (`uno-button.png`):
```text
A big shiny 3D red arcade buzzer button, thick polished metallic chrome ring rim, glossy glass dome surface with text "CALL UNO" embossed in golden yellow letters, game show buzzer, top angled perspective, vibrant studio lighting, isolated on transparent background --no background
```

### Prompt cho Bộ bài UNO (`cards/*.png`):
```text
Clean 2D vector style UNO playing card, rounded rectangle corners, thick white outer border, vibrant red card with a white diagonal tilted oval in the middle, crisp bold number "7" centered inside the oval with slight drop shadow, miniature matching numbers in top-left and bottom-right corners, official boardgame card aesthetic, isolated on transparent background --no background
```

### Prompt cho Thẻ chế độ (`modes/*.png`):
Đổi phần trong ngoặc vuông cho từng chế độ (xem bảng mục 3.6):
```text
Vertical playing-card shaped game mode tile, 2:3 aspect ratio, thick white rounded border, vibrant [RED] glossy background, [a classic UNO card with a tilted white oval] illustration in the center, bold chunky tilted cartoon title text "[UNO CLASSIC]" in yellow with thick black outline, 3D casual mobile game UI style, soft glow, sparkles, high detail, isolated on transparent background --ar 2:3 --no background
```

---

## 6. Cách kích hoạt ảnh trong mã nguồn (`assets.js`)

Khi đã lưu ảnh vào thư mục `client/public/images/`, bạn chỉ cần mở file `client/src/config/assets.js` và cập nhật các đường dẫn từ `null` thành đường dẫn ảnh tương ứng:

```javascript
// client/src/config/assets.js
export const ASSETS = {
  backgrounds: {
    table: '/images/backgrounds/table.jpg',
    menu: '/images/backgrounds/menu.jpg',
    login: '/images/backgrounds/login.jpg',
  },

  logo: '/images/ui/logo.png',
  cardBack: '/images/cards/back.png',

  // Hàm tự động ghép tên file lá bài dựa vào màu và giá trị
  cardFace: (card) => `/images/cards/${card.color}_${card.value}.png`,

  avatars: {
    'p-me': '/images/avatars/me.png',
    'p-ma': '/images/avatars/minh-anh.png',
    'p-hk': '/images/avatars/hoang-khang.png',
    'p-tn': '/images/avatars/thao-nguyen.png',
    'p-ql': '/images/avatars/quoc-long.png',
  },

  unoButton: '/images/ui/uno-button.png',
};
```

> **Lưu ý:**
> - Nền bàn chơi, nền menu, lá bài, thẻ chế độ bắt buộc có ảnh (không còn bản vẽ CSS). Chỉ logo, nền đăng nhập, avatar, nút UNO được để `null` — khi đó hệ thống dùng bản vẽ CSS.
