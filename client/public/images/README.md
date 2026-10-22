# Thư mục Hình ảnh (Assets) - UNO Online

Thư mục này chứa toàn bộ các tệp hình ảnh tĩnh được phục vụ cho client UNO (ReactJS + Vite).

Xem toàn bộ quy chuẩn kích thước, tỷ lệ, phong cách nghệ thuật, danh sách 54 lá bài chi tiết và câu lệnh Prompt AI mẫu tại:
👉 **[IMAGE_SPECIFICATIONS.md](../../IMAGE_SPECIFICATIONS.md)**

### Cấu trúc thư mục:
- `backgrounds/`: Hình nền bàn chơi (`table.jpg`), sảnh/menu (`menu.jpg`), đăng nhập (`login.jpg`)
- `cards/`: Mặt sau lá bài (`back.png`) và 54 mặt trước lá bài (`{color}_{value}.png`)
- `ui/`: Logo game (`logo.png`), nút bấm Hô UNO (`uno-button.png`)
- `avatars/`: Ảnh đại diện người chơi và bot (`me.png`, `minh-anh.png`, ...)

Sau khi thêm ảnh vào đây, hãy mở file `client/src/config/assets.js` để kích hoạt đường dẫn ảnh tương ứng.
