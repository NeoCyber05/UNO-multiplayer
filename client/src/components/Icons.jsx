const base = (size, sw = 2.2) => ({
  width: size,
  height: size,
  viewBox: '0 0 24 24',
  fill: 'none',
  stroke: 'currentColor',
  strokeWidth: sw,
  strokeLinecap: 'round',
  strokeLinejoin: 'round',
  'aria-hidden': true,
});

export const IconBack = ({ size = 18 }) => (
  <svg {...base(size, 2.4)}><path d="M15 18l-6-6 6-6" /></svg>
);
export const IconArrowRight = ({ size = 18, sw = 2.4 }) => (
  <svg {...base(size, sw)}><path d="M9 6l6 6-6 6" /></svg>
);
export const IconLogout = ({ size = 18 }) => (
  <svg {...base(size)}><path d="M15 3h4a2 2 0 012 2v14a2 2 0 01-2 2h-4" /><path d="M10 17l5-5-5-5" /><path d="M15 12H3" /></svg>
);
export const IconChat = ({ size = 18 }) => (
  <svg {...base(size, 2.1)}><path d="M21 11.5a8.4 8.4 0 01-8.9 8.5 9 9 0 01-4-1L3 20l1.2-4.5A8.4 8.4 0 015 5.5 8.4 8.4 0 0112.9 3a8.4 8.4 0 018.1 8.5z" /></svg>
);
export const IconHome = ({ size = 19 }) => (
  <svg {...base(size, 2)}><path d="M4 11l8-7 8 7" /><path d="M6 10v9a1 1 0 001 1h4v-6h2v6h4a1 1 0 001-1v-9" /></svg>
);
/** Robot; level đổi dáng: easy tròn cười, normal vuông trung tính, hard góc cạnh có sừng, mắt xếch. */
export const IconBot = ({ size = 19, level = 'normal' }) => {
  const dot = { fill: 'currentColor', stroke: 'none' };
  if (level === 'easy') {
    return (
      <svg {...base(size, 2)}>
        <path d="M12 3.5v2.5" /><circle cx="12" cy="2.8" r="1.2" {...dot} />
        <rect x="3.5" y="6" width="17" height="13" rx="6.5" />
        <circle cx="9" cy="11.5" r="1.6" {...dot} /><circle cx="15" cy="11.5" r="1.6" {...dot} />
        <path d="M9 14.8c1.6 1.4 4.4 1.4 6 0" />
      </svg>
    );
  }
  if (level === 'hard') {
    return (
      <svg {...base(size, 2)}>
        <path d="M6 7L4 2.5 9 6M18 7l2-4.5L15 6" />
        <path d="M5 6.5h14l1.5 3v7L18 20H6l-2.5-3.5v-7z" />
        <path d="M7.5 10l3.5 1.5M16.5 10L13 11.5" />
        <path d="M8.5 12.5h2.5v1.5H9zM15.5 12.5H13v1.5h2z" {...dot} />
        <path d="M9 17h6M11 16v2M13 16v2" />
      </svg>
    );
  }
  return (
    <svg {...base(size, 2)}>
      <path d="M12 3v3" /><circle cx="12" cy="2.6" r="0.9" {...dot} />
      <rect x="4" y="6" width="16" height="12" rx="4" />
      <circle cx="9" cy="12" r="1.4" {...dot} /><circle cx="15" cy="12" r="1.4" {...dot} />
      <path d="M10 15.5h4" /><path d="M2 11v3M22 11v3" /><path d="M9 18v2.5M15 18v2.5" />
    </svg>
  );
};
export const IconUsers = ({ size = 19 }) => (
  <svg {...base(size, 2)}><circle cx="9" cy="8" r="3" /><path d="M3 20c0-3.3 2.7-6 6-6s6 2.7 6 6" /><circle cx="17" cy="9" r="2.4" /><path d="M15.5 13.2c2.4.4 4.5 2.4 4.5 5.8" /></svg>
);
export const IconShop = ({ size = 19 }) => (
  <svg {...base(size, 2)}><path d="M4 9l1.5-5h13L20 9" /><path d="M4 9h16v9a2 2 0 01-2 2H6a2 2 0 01-2-2V9z" /><path d="M9 13a3 3 0 006 0" /></svg>
);
export const IconUser = ({ size = 19 }) => (
  <svg {...base(size, 2)}><circle cx="12" cy="8" r="4" /><path d="M4 20c0-4.4 3.6-7 8-7s8 2.6 8 7" /></svg>
);
export const IconTrophy = ({ size = 19 }) => (
  <svg {...base(size, 2)}><path d="M8 21h8M12 17v4M7 4h10v5a5 5 0 01-10 0V4z" /><path d="M7 6H4a3 3 0 003 4M17 6h3a3 3 0 01-3 4" /></svg>
);
export const IconGear = ({ size = 22 }) => (
  <svg {...base(size, 2.2)}>
    <circle cx="12" cy="12" r="3.2" />
    <path d="M19.4 15a1.7 1.7 0 00.3 1.8l.1.1a2 2 0 11-2.8 2.8l-.1-.1a1.7 1.7 0 00-1.8-.3 1.7 1.7 0 00-1 1.5V21a2 2 0 11-4 0v-.1a1.7 1.7 0 00-1.1-1.5 1.7 1.7 0 00-1.8.3l-.1.1a2 2 0 11-2.8-2.8l.1-.1a1.7 1.7 0 00.3-1.8 1.7 1.7 0 00-1.5-1H3a2 2 0 110-4h.1a1.7 1.7 0 001.5-1.1 1.7 1.7 0 00-.3-1.8l-.1-.1a2 2 0 112.8-2.8l.1.1a1.7 1.7 0 001.8.3H9a1.7 1.7 0 001-1.5V3a2 2 0 114 0v.1a1.7 1.7 0 001 1.5 1.7 1.7 0 001.8-.3l.1-.1a2 2 0 112.8 2.8l-.1.1a1.7 1.7 0 00-.3 1.8V9a1.7 1.7 0 001.5 1H21a2 2 0 110 4h-.1a1.7 1.7 0 00-1.5 1z" />
  </svg>
);
export const IconPlus = ({ size = 18 }) => (
  <svg {...base(size, 2.4)}><path d="M12 5v14M5 12h14" /></svg>
);
export const IconChip = ({ size = 30 }) => (
  <svg {...base(size, 2)}><rect x="6" y="6" width="12" height="12" rx="2" /><path d="M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3" /></svg>
);
export const IconSide = ({ size = 30 }) => (
  <svg {...base(size, 2)}><rect x="3" y="9" width="8" height="9" rx="2" /><rect x="13" y="9" width="8" height="9" rx="2" /><path d="M7 9V7a2 2 0 012-2h6a2 2 0 012 2v2" /></svg>
);
export const IconClock = ({ size = 20 }) => (
  <svg {...base(size, 2.2)}><circle cx="12" cy="13" r="8" /><path d="M12 9v4l2.5 2M9 2h6M12 2v3" /></svg>
);
export const IconCopy = ({ size = 15 }) => (
  <svg {...base(size)}><rect x="9" y="9" width="11" height="11" rx="2" /><path d="M5 15V5a2 2 0 012-2h10" /></svg>
);
export const IconShare = ({ size = 16 }) => (
  <svg {...base(size)}><circle cx="6" cy="12" r="2.2" /><circle cx="18" cy="5" r="2.2" /><circle cx="18" cy="19" r="2.2" /><path d="M8 10.8l8-4.6M8 13.2l8 4.6" /></svg>
);
export const IconCheck = ({ size = 13 }) => (
  <svg {...base(size, 3)}><path d="M5 13l4 4L19 7" /></svg>
);
export const IconSmile = ({ size = 26 }) => (
  <svg {...base(size, 2)}><circle cx="12" cy="12" r="9" /><path d="M8 14s1.5 2 4 2 4-2 4-2" /><path d="M9 9.5h.01M15 9.5h.01" strokeWidth="3" /></svg>
);
export const IconPlay = ({ size = 18 }) => (
  <svg width={size} height={size} viewBox="0 0 24 24" fill="currentColor" aria-hidden><path d="M8 5.5v13a1 1 0 001.5.9l10.4-6.5a1 1 0 000-1.8L9.5 4.6A1 1 0 008 5.5z" /></svg>
);
export const IconClose = ({ size = 18 }) => (
  <svg {...base(size, 2.4)}><path d="M6 6l12 12M18 6L6 18" /></svg>
);
export const IconSound = ({ size = 20 }) => (
  <svg {...base(size, 2.1)}><path d="M4 9v6h4l5 4V5L8 9H4z" /><path d="M16.5 8.5a5 5 0 010 7M19 6a8.5 8.5 0 010 12" /></svg>
);
export const IconMute = ({ size = 20 }) => (
  <svg {...base(size, 2.1)}><path d="M4 9v6h4l5 4V5L8 9H4z" /><path d="M17 9.5l5 5M22 9.5l-5 5" /></svg>
);
export const IconBook = ({ size = 22 }) => (
  <svg {...base(size, 2)}><path d="M4 5a2 2 0 012-2h13v16H6a2 2 0 00-2 2V5z" /><path d="M4 19a2 2 0 012-2h13" /><path d="M9 7h6" /></svg>
);

export const IconEdit = ({ size = 16 }) => (
  <svg {...base(size, 2.1)}><path d="M4 20h4L19 9l-4-4L4 16v4z" /><path d="M13.5 6.5l4 4" /></svg>
);
export const IconMusic = ({ size = 20 }) => (
  <svg {...base(size, 2)}><path d="M9 18V5l11-2v13" /><circle cx="6.5" cy="18" r="2.5" /><circle cx="17.5" cy="16" r="2.5" /></svg>
);
export const IconCards = ({ size = 20 }) => (
  <svg {...base(size, 2)}><rect x="3" y="6" width="11" height="15" rx="2" transform="rotate(-8 8.5 13.5)" /><path d="M13 3.5l5.5 1a2 2 0 011.6 2.3L18 19" /></svg>
);
export const IconMegaphone = ({ size = 20 }) => (
  <svg {...base(size, 2)}><path d="M3 10v4a1 1 0 001 1h3l8 4.5V4.5L7 9H4a1 1 0 00-1 1z" /><path d="M7 15l1.5 5h3L10 15" /><path d="M19 9a4 4 0 010 6" /></svg>
);
export const IconHandshake = ({ size = 20 }) => (
  <svg {...base(size, 2)}><path d="M2 8l4-2 4 1.5" /><path d="M22 8l-4-2-5 2-3.5 3a1.6 1.6 0 002.2 2.3L14 11.5l4.5 4.5" /><path d="M2 8v7l5.5 4.5a1.5 1.5 0 002.1-.2" /><path d="M22 8v7l-3.5 1" /><path d="M9 17l1.5 1.5a1.5 1.5 0 002.1 0" /><path d="M12 15l1.8 1.8a1.5 1.5 0 002.1 0" /></svg>
);
export const IconDiamond = ({ size = 20 }) => (
  <svg {...base(size, 2)}><path d="M6.5 4h11L22 9.5 12 21 2 9.5z" /><path d="M2 9.5h20" /><path d="M9.5 4L8 9.5l4 11.5 4-11.5L14.5 4" /></svg>
);
export const IconCrown = ({ size = 18 }) => (
  <svg {...base(size, 2)}><path d="M3 8l4.5 4L12 5l4.5 7L21 8l-2 11H5z" fill="currentColor" fillOpacity="0.25" /></svg>
);
export const IconSwords = ({ size = 18 }) => (
  <svg {...base(size, 2)}><path d="M14.5 17.5L3 6V3h3l11.5 11.5" /><path d="M13 19l6-6M16 16l4 4M19 21l2-2" /><path d="M9.5 14.5L3 21M14.5 6.5L18 3h3v3l-3.5 3.5" /><path d="M5 14l5 5" /></svg>
);
export const IconShield = ({ size = 20 }) => (
  <svg {...base(size, 2)}><path d="M12 3l8 3v6c0 4.5-3.4 8.2-8 9-4.6-.8-8-4.5-8-9V6l8-3z" /><path d="M9 12l2 2 4-4" /></svg>
);
export const IconFlame = ({ size = 18 }) => (
  <svg {...base(size, 2)}><path d="M12 21c-4 0-7-2.7-7-6.5 0-3.5 3-5.5 3-9 2.5 1.5 4 3.5 4.5 5.5 1-1 1.5-2.3 1.5-3.5 2.5 2 5 4.5 5 7.5 0 3.5-3 6-7 6z" /></svg>
);
