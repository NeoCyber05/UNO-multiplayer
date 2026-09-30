// Các chế độ hiện trên carousel ở Sảnh. Thêm chế độ mới = thêm 1 phần tử.
// image: ảnh thẻ PNG 600x900 (tỉ lệ 2:3) trong public/images/modes/, sinh bởi scripts/gen_modes.py.
export const LOBBY_MODES = [
  {
    key: '2v2',
    title: '2 vs 2',
    desc: 'Đồng đội ngồi đối diện qua bàn, bài của nhau luôn hiện ra để phối hợp nước đi.',
    chip: 'XẾP HẠNG · ELO',
    ranked: true,
    image: '/images/modes/2v2.png',
    to: '/room/2v2',
  },
  {
    key: 'classic',
    title: 'Classic',
    desc: 'UNO truyền thống: 4 người, mỗi người một mình, ai hết bài trước thắng.',
    chip: 'XẾP HẠNG · ELO',
    ranked: true,
    image: '/images/modes/classic.png',
    to: '/matchmaking/classic',
  },
  {
    key: 'side',
    title: 'Side to Side',
    desc: 'Đồng đội ngồi ngay bên trái bạn, hai người nhìn thấy bài của nhau.',
    chip: 'XẾP HẠNG · ELO',
    ranked: true,
    image: '/images/modes/side.png',
    to: '/room/side',
  },
  {
    key: 'custom',
    title: 'Custom',
    desc: 'Phòng riêng: mời bạn bè hoặc thêm bot, tự chọn luật chơi.',
    chip: 'KHÔNG TÍNH ELO',
    ranked: false,
    image: '/images/modes/custom.png',
    to: '/custom',
  },
];

// Thẻ nằm giữa khi mở Sảnh
export const DEFAULT_MODE = 'classic';
