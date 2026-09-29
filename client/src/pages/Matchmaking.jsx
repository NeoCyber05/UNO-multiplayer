import { useEffect, useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconBack, IconClock } from '../components/Icons.jsx';
import { useSocial } from '../context/SocialContext.jsx';
import { STRANGERS } from '../data/mockData';
import { MODES } from '../game/constants';
import { shuffle } from '../game/deck';
import './pages.css';

// Giả lập: ghế trống thứ k tìm được người sau FOUND_AT[k] giây. Thay bằng sự kiện socket khi có server.
const FOUND_AT = [2, 4, 6];
const START_DELAY = 3;

export default function Matchmaking() {
  const { mode } = useParams();
  const cfg = MODES[mode] ?? MODES.classic;
  const navigate = useNavigate();
  const { me, party } = useSocial();
  const [sec, setSec] = useState(0);
  const back = cfg.teams ? `/room/${cfg.key}` : '/lobby';

  // Đội hình theo ghế: bạn + đồng đội (nếu đã tổ đội) giữ nguyên, ghế còn lại là người lạ.
  const [plan] = useState(() => {
    const teammate = cfg.teams && party.mode === cfg.key ? party.teammate : null;
    const strangers = shuffle(STRANGERS);
    let k = 0;
    const lineup = [0, 1, 2, 3].map((i) => {
      if (i === 0) return me;
      if (teammate && i === cfg.teammateSeat) return teammate;
      return strangers[k++];
    });
    const preset = [0, 1, 2, 3].map((i) => i === 0 || (!!teammate && i === cfg.teammateSeat));
    return { lineup, preset, teammateSeat: teammate ? cfg.teammateSeat : null };
  });

  useEffect(() => {
    const t = setInterval(() => setSec((s) => s + 1), 1000);
    return () => clearInterval(t);
  }, []);

  let k = 0;
  const found = plan.lineup.map((p, i) => (plan.preset[i] || sec >= FOUND_AT[k++] ? p : null));
  const count = found.filter(Boolean).length;
  const ready = count === 4;
  const lastFound = FOUND_AT[plan.preset.filter((x) => !x).length - 1];
  const startIn = ready ? Math.max(0, lastFound + START_DELAY - sec) : null;

  const start = () => navigate(`/game/${cfg.key}`, { state: { lineup: plan.lineup, ranked: true } });

  useEffect(() => {
    if (ready && startIn === 0) start();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, startIn]);

  return (
    <div className="screen">
      <MenuBackground />
      <header className="page-head">
        <button type="button" className="icon-btn" onClick={() => navigate(back)} aria-label="Quay lại">
          <IconBack />
        </button>
        <div className="display page-head__title">Ghép trận</div>
      </header>

      <div className="mm">
        <div className={`radar ${ready ? 'is-ready' : ''}`}>
          <span className="radar__wave" />
          <span className="radar__wave" style={{ animationDelay: '0.8s' }} />
          <span className="radar__wave" style={{ animationDelay: '1.6s' }} />
          <span className="radar__ring" style={{ inset: 0 }} />
          <span className="radar__ring" style={{ inset: 40 }} />
          <span className="radar__ring" style={{ inset: 80 }} />
          {!ready && <span className="radar__sweep" />}
          <Avatar person={me} size={96} className="radar__me" />
        </div>

        <div style={{ textAlign: 'center' }}>
          <div className="display mm__title">
            {ready ? 'Đã tìm đủ người chơi!' : `Đang tìm đối thủ cho chế độ ${cfg.label}…`}
          </div>
          <div className="muted" style={{ fontSize: 14, marginTop: 6 }}>
            {ready ? `Trận đấu bắt đầu sau ${startIn} giây` : 'Thời gian chờ trung bình ~15 giây'}
          </div>
        </div>

        <div className="mm__clock display">
          <IconClock size={22} />
          {String(Math.floor(sec / 60)).padStart(2, '0')}:{String(sec % 60).padStart(2, '0')}
        </div>

        <div className="mm__slots">
          {found.map((p, i) => (
            <div key={i} className={`mm__slot ${p ? 'is-found' : ''}`}>
              <Avatar person={p} size={64} />
              <span>{p ? p.name : 'Đang tìm…'}</span>
              {i === plan.teammateSeat && <span className="chip seat-chip seat-chip--team">Đồng đội</span>}
            </div>
          ))}
        </div>
        <div className="muted" style={{ fontSize: 12 }}>Đã tìm {count}/4 người chơi</div>

        <div style={{ display: 'flex', gap: 16, marginTop: 4 }}>
          <button type="button" className="btn btn--danger" onClick={() => navigate(back)}>
            Huỷ tìm trận
          </button>
          <button type="button" className="btn btn--primary" disabled={!ready} onClick={start}>
            Vào trận ngay
          </button>
        </div>
      </div>
    </div>
  );
}
