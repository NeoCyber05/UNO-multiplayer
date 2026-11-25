import { useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconBack } from '../components/Icons.jsx';
import { asPerson, useSocial } from '../context/SocialContext.jsx';
import { useSession } from '../net/session';
import { MODES } from '../game/constants';
import './pages.css';

export default function Matchmaking() {
  const { mode } = useParams();
  const cfg = MODES[mode] ?? MODES.classic;
  const navigate = useNavigate();
  const { me } = useSocial();
  const { queue, request, receive, connection } = useSession();
  const [pending, setPending] = useState(false);
  const [error, setError] = useState('');
  const waiting = queue?.mode === cfg.key && queue.active === true;
  const back = cfg.teams ? `/room/${cfg.key}` : '/lobby';
  const players = waiting ? queue.players ?? [] : [];
  const ready = players.filter(Boolean).length === 4;

  const command = async (leave) => {
    if (pending) return;
    if (leave && !waiting) return navigate(back);
    setError('');
    setPending(true);
    try {
      if (leave) {
        await request('QUEUE_LEAVE');
        navigate(back);
      } else {
        const response = await request('QUEUE_JOIN', { mode: cfg.key });
        if (response.queue) receive('QUEUE_STATE', response.queue);
      }
    } catch (error) {
      setError(error.message);
    } finally {
      setPending(false);
    }
  };

  return (
    <div className="screen">
      <MenuBackground />
      <header className="page-head">
        <button type="button" className="icon-btn" disabled={pending} onClick={() => command(true)} aria-label="Quay lại"><IconBack /></button>
        <div className="display page-head__title">Ghép trận</div>
      </header>
      <div className="mm">
        <div className={`radar ${ready ? 'is-ready' : ''}`}>
          <span className="radar__ring" style={{ inset: 0 }} />
          <span className="radar__ring" style={{ inset: 40 }} />
          {waiting && !ready && <span className="radar__sweep" />}
          <Avatar person={me} size={96} className="radar__me" />
        </div>
        <div className="display mm__title">
          {ready ? 'Đã tìm đủ người chơi' : waiting ? `Đang tìm trận ${cfg.label}…` : cfg.label}
        </div>
        {waiting && <div className="mm__slots">
          {Array.from({ length: 4 }, (_, index) => {
            const player = players[index];
            return <div key={index} className={`mm__slot ${player ? 'is-found' : ''}`}>
              <Avatar person={player ? asPerson(player) : null} size={64} />
              <span>{player?.name ?? 'Đang tìm…'}</span>
            </div>;
          })}
        </div>}
        {error && <p role="alert">{error}</p>}
        <button type="button" disabled={pending || connection !== 'connected'}
          className={`btn btn--${waiting ? 'danger' : 'primary'}`} onClick={() => command(waiting)}>
          {waiting ? 'Huỷ tìm trận' : 'Tìm trận'}
        </button>
      </div>
    </div>
  );
}
