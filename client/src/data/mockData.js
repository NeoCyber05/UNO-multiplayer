// Dữ liệu mẫu — sau này thay bằng dữ liệu thật từ server.

export const ME = { id: 'p-me', name: 'Bách', initials: 'BC', tint: '#4C86FF', level: 12, xp: 68, elo: 1491 };

export const BOTS = [
  { id: 'p-ma', name: 'Minh Anh', initials: 'MA', tint: '#FF5262' },
  { id: 'p-hk', name: 'Hoàng Khang', initials: 'HK', tint: '#22C3D6' },
  { id: 'p-tn', name: 'Thảo Nguyên', initials: 'TN', tint: '#35D399' },
];

// Độ khó bot: key lưu trong person.botLevel, tint = màu avatar + ô chọn độ khó
export const BOT_LEVELS = [
  { key: 'easy', label: 'Dễ', tint: '#2FA37A' },
  { key: 'normal', label: 'Thường', tint: '#3A7CA5' },
  { key: 'hard', label: 'Khó', tint: '#D8434F' },
];

export const QUICK_CHAT = ['Chơi hay lắm!', 'Nhanh lên nào!', "Let's win!", 'Hên thôi 😅', '😂', '😎', '😡', '👍'];

export const BADGES = [
    { key: 'first-win', icon: 'trophy', tier: 'bronze', name: 'Chiến thắng đầu tiên', desc: 'Thắng 1 ván bất kỳ', stat: 'wins', goal: 1 },
    { key: 'win-100', icon: 'crown', tier: 'gold', name: 'Bách chiến bách thắng', desc: 'Thắng 100 trận', stat: 'wins', goal: 100 },
    { key: 'streak-10', icon: 'flame', tier: 'silver', name: 'Chuỗi 10 trận thắng', desc: 'Thắng liên tiếp 10 trận', stat: 'bestStreak', goal: 10 },
    { key: 'plus4', icon: 'cards', tier: 'bronze', name: 'Ném 50 lá +4', desc: 'Đánh 50 lá Wild +4', stat: 'plus4', goal: 50 },
    { key: 'uno-master', icon: 'megaphone', tier: 'silver', name: 'Hô UNO 300 lần', desc: 'Hô UNO đúng lúc 300 lần', stat: 'unoCalls', goal: 300 },
    { key: 'team', icon: 'handshake', tier: 'gold', name: 'Đồng đội ăn ý', desc: 'Thắng 50 trận đồng đội', stat: 'teamWins', goal: 50 },
    { key: 'matches-500', icon: 'shield', tier: 'silver', name: 'Cựu binh bàn bài', desc: 'Chơi 500 trận', stat: 'matches', goal: 500 },
    { key: 'diamond', icon: 'diamond', tier: 'diamond', name: 'Đạt hạng Kim cương', desc: 'Đạt 1.800 ELO', stat: 'peakElo', goal: 1800 },
  ];
