export const RANKS = [
  { key: 'bronze', name: 'Đồng', min: 0, color: '#a26837' },
  { key: 'silver', name: 'Bạc', min: 1200, color: '#667e91' },
  { key: 'gold', name: 'Vàng', min: 1400, color: '#a77a1d' },
  { key: 'platinum', name: 'Bạch kim', min: 1600, color: '#25867a' },
  { key: 'diamond', name: 'Kim cương', min: 1800, color: '#397fbd' },
  { key: 'master', name: 'Cao thủ', min: 2000, color: '#ba6650' },
];
export const rankOf = elo => RANKS[Math.max(0, RANKS.findLastIndex(rank => elo >= rank.min))];
