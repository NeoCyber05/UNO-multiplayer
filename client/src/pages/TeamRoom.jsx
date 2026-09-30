import { useEffect, useRef, useState } from 'react';
import { Navigate, useNavigate, useParams } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconBack, IconClose, IconPlay } from '../components/Icons.jsx';
import { useSocial } from '../context/SocialContext.jsx';
import { MODES, SEATS } from '../game/constants';
import './pages.css';

const TEAM_MODES = ['2v2', 'side'];
const INVITE_REPLY_MS = 2000;
const HINT = {
  '2v2': 'Đồng đội ngồi đối diện — bài của nhau luôn hiện ra với bạn',
  side: 'Đồng đội ngồi ngay bên trái bạn — hai người nhìn thấy bài của nhau',
};

// Giả lập: bạn đang trong trận thì từ chối lời mời. Thay bằng phản hồi socket khi có server.
const isBusy = (f) => f.status?.includes('trong trận');

/** Phòng tổ đội 2 vs 2 / Side to Side: mời 1 bạn làm đồng đội rồi đi ghép trận tìm đối thủ. */
export default function TeamRoom() {
  const { mode } = useParams();
  if (!TEAM_MODES.includes(mode)) return <Navigate to="/matchmaking/classic" replace />;
  return <Room key={mode} mode={mode} />;
}

function Room({ mode }) {
  const cfg = MODES[mode];
  const navigate = useNavigate();
  const { me, friends, party, setParty } = useSocial();
  const [pendingId, setPendingId] = useState(null);
  const [toast, setToast] = useState(null);
  const timer = useRef(null);

  const teammate = party.mode === mode ? party.teammate : null;
  const notify = (text) => setToast({ id: Date.now(), text });

  // Vào phòng của chế độ khác -> bỏ nhóm cũ
  useEffect(() => {
    if (party.mode !== mode) setParty({ mode, teammate: null });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [mode]);

  useEffect(() => () => clearTimeout(timer.current), []);

  const list = friends
    .filter((f) => !f.pending)
    .sort((a, b) => Number(b.online) - Number(a.online));
  const onlineCount = list.filter((f) => f.online).length;

  const invite = (f) => {
    setPendingId(f.id);
    timer.current = setTimeout(() => {
      setPendingId(null);
      if (isBusy(f)) {
        notify(`${f.name} đang trong trận, chưa thể vào phòng`);
        return;
      }
      setParty({ mode, teammate: f });
      notify(`${f.name} đã vào phòng`);
    }, INVITE_REPLY_MS);
  };

  const cancelInvite = () => {
    clearTimeout(timer.current);
    setPendingId(null);
    notify('Đã huỷ lời mời');
  };

  const kick = () => {
    notify(`${teammate.name} đã rời phòng`);
    setParty({ mode, teammate: null });
  };

  const seatOf = (i) => {
    if (i === 0) return { kind: 'me', person: me };
    if (i === cfg.teammateSeat) return { kind: 'mate', person: teammate };
    return { kind: 'enemy', person: null };
  };

  const friendButton = (f) => {
    if (!f.online) return null;
    if (teammate?.id === f.id) {
      return <button type="button" className="btn btn--sm btn--outline" disabled>Trong phòng</button>;
    }
    if (pendingId === f.id) {
      return (
        <button type="button" className="btn btn--sm btn--outline is-waiting" onClick={cancelInvite} title="Bấm để huỷ lời mời">
          Đã mời…
        </button>
      );
    }
    const blocked = !!teammate || !!pendingId;
    return (
      <button
        type="button"
        className="btn btn--sm btn--ghost"
        disabled={blocked}
        onClick={() => invite(f)}
        title={teammate ? 'Phòng đã đủ đồng đội' : pendingId ? 'Đang chờ phản hồi lời mời khác' : undefined}
      >
        Mời
      </button>
    );
  };

  return (
    <div className="screen">
      <MenuBackground />

      <header className="page-head">
        <button type="button" className="icon-btn" onClick={() => navigate('/lobby')} aria-label="Quay lại">
          <IconBack />
        </button>
        <div style={{ flexGrow: 1 }}>
          <div className="display page-head__title">Phòng chờ · {cfg.label}</div>
          <div className="muted" style={{ fontSize: 13, marginTop: 2 }}>{HINT[mode]}</div>
        </div>
      </header>

      <div className="room">
        <div className="room__table-wrap">
          <div className="room-table">
            <div className="room-table__felt"><span className="display">UNO</span></div>
            {SEATS.map((pos, i) => {
              const seat = seatOf(i);
              const { person } = seat;
              const rel = seat.kind === 'enemy' ? 'is-enemy' : 'is-team';
              return (
                <div key={pos} className={`room-seat room-seat--${pos} ${person ? '' : 'is-empty'} ${rel}`}>
                  {seat.kind === 'mate' && person && (
                    <button type="button" className="seat-remove" onClick={kick} aria-label="Mời ra khỏi phòng">
                      <IconClose size={14} />
                    </button>
                  )}
                  <Avatar person={person} size={56} />
                  <div className="room-seat__name">
                    {seat.kind === 'me' && `${person.name} (Bạn)`}
                    {seat.kind === 'mate' && (person ? person.name : pendingId ? 'Đang chờ phản hồi…' : 'Ghế đồng đội')}
                    {seat.kind === 'enemy' && 'Ghép trận sẽ tìm'}
                  </div>
                  {seat.kind === 'me' && <span className="chip seat-chip seat-chip--host">Chủ phòng</span>}
                  {seat.kind === 'mate' && (
                    <span className="chip seat-chip seat-chip--team">{person ? 'Đồng đội' : 'Mời bạn bè →'}</span>
                  )}
                  {seat.kind === 'enemy' && <span className="chip seat-chip seat-chip--enemy">Đối thủ</span>}
                </div>
              );
            })}
          </div>

          <div className="room__legend">
            <span><i className="dot" style={{ background: 'var(--green)' }} /> Đội của bạn</span>
            <span><i className="dot" style={{ background: 'var(--red)' }} /> Đội đối thủ</span>
          </div>
        </div>

        <div className="room__side">
          <div className="panel room__box">
            <div className="section-title">BẠN BÈ · {onlineCount} ONLINE</div>
            <div className="friend-list team-friends">
              {list.map((f) => (
                <div key={f.id} className={`friend ${f.online ? '' : 'is-offline'}`}>
                  <div className="friend__avatar">
                    <Avatar person={f} size={40} />
                    <span className={`friend__status ${f.online ? 'is-on' : ''}`} />
                  </div>
                  <div style={{ flexGrow: 1, minWidth: 0 }}>
                    <div style={{ fontWeight: 700, fontSize: 14 }}>{f.name}</div>
                    <div className="muted friend__sub">{f.online ? f.status : f.status || 'Offline'}</div>
                  </div>
                  {friendButton(f)}
                </div>
              ))}
            </div>
          </div>

          <div style={{ flexGrow: 1 }} />

          <div className="muted" style={{ fontSize: 13, textAlign: 'center' }}>
            {teammate ? `Đi cùng ${teammate.name} · ghép trận tìm 2 đối thủ` : 'Chưa có đồng đội · ghép trận sẽ tìm ngẫu nhiên'}
          </div>
          <button
            type="button"
            className="btn btn--primary btn--block"
            disabled={!!pendingId}
            onClick={() => navigate(`/matchmaking/${mode}`)}
            title={pendingId ? 'Đang chờ phản hồi lời mời' : undefined}
          >
            <IconPlay /> Tìm trận
          </button>
        </div>
      </div>

      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}
