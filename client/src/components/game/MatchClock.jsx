import { useEffect, useState } from 'react';
import { IconClock } from '../Icons.jsx';

const fmt = (sec) => `${String(Math.floor(sec / 60)).padStart(2, '0')}:${String(sec % 60).padStart(2, '0')}`;

/** Đồng hồ thời gian ván đấu (góc trên trái). */
export default function MatchClock({ startedAt, stopped }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    if (stopped) return undefined;
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, [stopped]);

  return (
    <div className="hud-clock">
      <span className="hud-clock__icon"><IconClock size={22} /></span>
      {fmt(Math.max(0, Math.floor((now - startedAt) / 1000)))}
    </div>
  );
}
