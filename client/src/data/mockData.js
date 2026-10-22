// Dữ liệu mẫu — sau này thay bằng dữ liệu thật từ server.

export const ME = { id: 'p-me', name: 'Bách', initials: 'BC', tint: '#4C86FF', level: 12, xp: 68, elo: 1491 };

export const BOTS = [
  { id: 'p-ma', name: 'Minh Anh', initials: 'MA', tint: '#FF5262' },
  { id: 'p-hk', name: 'Hoàng Khang', initials: 'HK', tint: '#22C3D6' },
  { id: 'p-tn', name: 'Thảo Nguyên', initials: 'TN', tint: '#35D399' },
];

export const FRIENDS = [
  { ...BOTS[2], elo: 1620, status: 'Đang ở sảnh chờ', online: true },
  { ...BOTS[1], elo: 1555, status: 'Đang trong trận · 2 vs 2', online: true },
  { ...BOTS[0], elo: 1402, status: 'Rảnh', online: true },
  { id: 'p-ql', name: 'Quốc Long', initials: 'QL', tint: '#FFC94A', elo: 1338, status: 'Offline 2 giờ trước', online: false },
];

// Người lạ ghép trận gặp (không phải bạn bè). Sau này server ghép theo ELO.
export const STRANGERS = [
  { id: 's-dt', name: 'Đình Trọng', initials: 'ĐT', tint: '#FF8A3D', elo: 1512 },
  { id: 's-lh', name: 'Lan Hương', initials: 'LH', tint: '#35D399', elo: 1468 },
  { id: 's-vk', name: 'Văn Khoa', initials: 'VK', tint: '#4C86FF', elo: 1530 },
  { id: 's-mt', name: 'Mai Trang', initials: 'MT', tint: '#FF5262', elo: 1445 },
  { id: 's-qh', name: 'Quang Huy', initials: 'QH', tint: '#22C3D6', elo: 1497 },
  { id: 's-ny', name: 'Như Ý', initials: 'NY', tint: '#FFC94A', elo: 1483 },
];

// Bảng xếp hạng ELO (đã sắp giảm dần). Sau này lấy từ server.
export const LEADERBOARD = [
  { id: 'lb-1', name: 'Khánh Linh', initials: 'KL', tint: '#FF5262', elo: 2140 },
  { id: 'lb-2', name: 'Đức Anh', initials: 'ĐA', tint: '#4C86FF', elo: 2055 },
  { id: 'lb-3', name: 'Phương Vy', initials: 'PV', tint: '#35D399', elo: 1987 },
  { id: 'lb-4', name: 'Tuấn Kiệt', initials: 'TK', tint: '#FFC94A', elo: 1902 },
  { id: 'lb-5', name: 'Gia Hân', initials: 'GH', tint: '#22C3D6', elo: 1811 },
  { id: 'lb-6', name: 'Minh Quân', initials: 'MQ', tint: '#FF8A3D', elo: 1740 },
  FRIENDS[0],
  FRIENDS[1],
  ME,
  FRIENDS[2],
];

// Độ khó bot: key lưu trong person.botLevel, tint = màu avatar + ô chọn độ khó
export const BOT_LEVELS = [
  { key: 'easy', label: 'Dễ', tint: '#2FA37A' },
  { key: 'normal', label: 'Thường', tint: '#3A7CA5' },
  { key: 'hard', label: 'Khó', tint: '#D8434F' },
];

export const QUICK_CHAT = ['Chơi hay lắm!', 'Nhanh lên nào!', "Let's win!", 'Hên thôi 😅', '😂', '😎', '😡', '👍'];

// Hồ sơ người chơi (ME). Sau này lấy từ server.
export const PROFILE = {
  tag: 'BACH#1406',
  joined: '14/06/2025',
  title: 'Vua đổi màu',
  stats: { matches: 248, wins: 131, streak: 4, bestStreak: 11, unoCalls: 312, plus4: 97 },
  modes: [
    { key: 'classic', title: 'Classic', matches: 112, wins: 57 },
    { key: '2v2', title: '2 vs 2', matches: 64, wins: 38 },
    { key: 'side', title: 'Side to Side', matches: 42, wins: 22 },
    { key: 'draft', title: 'UNO Draft', matches: 30, wins: 14 },
  ],
  // tier: bronze | silver | gold | diamond. Hoàn thành khi progress >= goal.
  badges: [
    { key: 'first-win', icon: 'trophy', tier: 'bronze', name: 'Chiến thắng đầu tiên', desc: 'Thắng 1 ván bất kỳ', progress: 1, goal: 1 },
    { key: 'win-100', icon: 'crown', tier: 'gold', name: 'Bách chiến bách thắng', desc: 'Thắng 100 trận', progress: 131, goal: 100 },
    { key: 'streak-10', icon: 'flame', tier: 'silver', name: 'Chuỗi 10 trận thắng', desc: 'Thắng liên tiếp 10 trận', progress: 11, goal: 10 },
    { key: 'plus4', icon: 'cards', tier: 'bronze', name: 'Ném 50 lá +4', desc: 'Đánh 50 lá Wild +4', progress: 97, goal: 50 },
    { key: 'uno-master', icon: 'megaphone', tier: 'silver', name: 'Hô UNO 300 lần', desc: 'Hô UNO đúng lúc 300 lần', progress: 312, goal: 300 },
    { key: 'team', icon: 'handshake', tier: 'gold', name: 'Đồng đội ăn ý', desc: 'Thắng 50 trận đồng đội', progress: 38, goal: 50 },
    { key: 'matches-500', icon: 'shield', tier: 'silver', name: 'Cựu binh bàn bài', desc: 'Chơi 500 trận', progress: 248, goal: 500 },
    { key: 'diamond', icon: 'diamond', tier: 'diamond', name: 'Đạt hạng Kim cương', desc: 'Đạt 2.200 ELO', progress: 1840, goal: 2200 },
  ],

};

// Lịch sử trận gần đây. delta: thay đổi ELO (null = không xếp hạng).
export const MATCH_HISTORY = [
  { id: 'm1', mode: 'Classic', place: 1, players: 4, delta: +24, when: '10 phút trước' },
  { id: 'm2', mode: '2 vs 2', place: 1, players: 4, delta: +18, when: '35 phút trước' },
  { id: 'm3', mode: 'Classic', place: 3, players: 4, delta: -12, when: '1 giờ trước' },
  { id: 'm4', mode: 'Custom', place: 2, players: 4, delta: null, when: 'Hôm qua' },
  { id: 'm5', mode: 'Side to Side', place: 2, players: 4, delta: -15, when: 'Hôm qua' },
];

// Bậc hạng theo ELO (min tăng dần).
export { RANKS } from './ranks';
