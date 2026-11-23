import { useEffect, useState } from 'react';
import { IconClock } from '../Icons.jsx';

const fmt = (sec) => `${String(Math.floor(sec / 60)).padStart(2, '0')}:${String(sec % 60).padStart(2, '0')}`;

/** Đồng hồ thời gian ván đấu (góc trên trái). */
export default function MatchClock({ mode, receivedAt, durationMs, stopped, drafting }) {
  const [now, setNow] = useState(performance.now());
  useEffect(() => {
    if (stopped || drafting) return undefined;
    setNow(performance.now());
    const t = setInterval(() => setNow(performance.now()), 1000);
    return () => clearInterval(t);
  }, [stopped, drafting, receivedAt]);

  const elapsed = durationMs + (stopped || drafting ? 0 : Math.max(0, now - receivedAt));

  return (
    <div className="hud-clock">
      <span className="hud-clock__icon"><IconClock size={22} /></span>
      <span className="hud-clock__mode">{mode}</span>
      <time>{fmt(Math.floor(elapsed / 1000))}</time>
    </div>
  );
}
