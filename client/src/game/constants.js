export const COLORS = ['red', 'yellow', 'green', 'blue'];

export const COLOR_LABEL = { red: 'Đỏ', yellow: 'Vàng', green: 'Xanh lá', blue: 'Xanh dương' };

// Màu hiển thị (khớp với biến CSS --uno-*)
export const COLOR_HEX = { red: '#E8212E', yellow: '#FFC700', green: '#1FA64A', blue: '#1768D8' };

// Thứ tự ghế theo index người chơi: 0 = bạn (dưới), đi theo chiều kim đồng hồ.
export const SEATS = ['bottom', 'left', 'top', 'right'];

export const MODES = {
  classic: { key: 'classic', label: 'Classic', accent: '#E8212E', teams: false, teammateSeat: null, teammateOpen: false },
  '2v2': { key: '2v2', label: '2 vs 2', accent: '#35D399', teams: true, teammateSeat: 2, teammateOpen: true },
  side: { key: 'side', label: 'Side to Side', accent: '#22C3D6', teams: true, teammateSeat: 1, teammateOpen: true },
};

export const HAND_SIZE = 7;
export const TURN_SECONDS = 20;
export const UNO_GRACE_MS = 3000;
