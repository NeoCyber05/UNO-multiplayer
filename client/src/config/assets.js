/**
 * ============================================================
 *  CẤU HÌNH HÌNH ẢNH (ASSETS)
 * ============================================================
 *  Tất cả ảnh đặt trong thư mục `client/public/images/...`
 *  rồi khai báo đường dẫn tại đây (bắt đầu bằng "/images/...").
 *
 *  Để `null` => dùng giao diện vẽ bằng CSS mặc định (không cần ảnh).
 *  Xem danh sách đầy đủ + kích thước gợi ý trong client/README.md
 * ============================================================
 */
export const ASSETS = {
  backgrounds: {
    // Nền bàn chơi (ảnh 1440x900 hoặc lớn hơn, tỉ lệ 16:10)
    // Sinh bởi client/scripts/gen_table_bg.py; tia sáng + đèn sân khấu CSS vẫn chạy phía trên
    table: '/images/backgrounds/table.jpg',
    // Nền các màn hình menu (sảnh, chọn chế độ, phòng chờ, ghép trận)
    // Sinh bởi client/scripts/gen_menu_bg.py
    menu: '/images/backgrounds/menu.jpg',
    // Nền cột trái màn hình đăng nhập
    login: null, // ví dụ: '/images/backgrounds/login.jpg'
  },

  // Logo game (PNG nền trong suốt, ~400x160)
  logo: null, // ví dụ: '/images/ui/logo.png'

  // Mặt sau lá bài (PNG, tỉ lệ 2:3, gợi ý 240x360)
  cardBack: '/images/cards/back.png',

  // Mặt trước lá bài: hàm nhận card => trả về đường dẫn ảnh.
  // card.color: 'red' | 'yellow' | 'green' | 'blue' | 'wild'
  // card.value: '0'..'9' | 'skip' | 'reverse' | 'draw2' | 'discard_all' | 'wild' | 'wild4'
  // Ảnh sinh bởi client/scripts/gen_cards.py
  cardFace: (card) => `/images/cards/${card.color}_${card.value}.png`,

  // Ảnh đại diện người chơi, theo id (xem src/data/mockData.js)
  avatars: {
    // 'p-me': '/images/avatars/me.png',
    // 'p-ma': '/images/avatars/minh-anh.png',
    // 'p-hk': '/images/avatars/hoang-khang.png',
    // 'p-tn': '/images/avatars/thao-nguyen.png',
    // 'p-ql': '/images/avatars/quoc-long.png',
  },

  // Nút CALL UNO (PNG tròn nền trong suốt, ~300x300)
  unoButton: null, // ví dụ: '/images/ui/uno-button.png'
};
