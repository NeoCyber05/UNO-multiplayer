import { useState } from 'react';
import { Navigate, useNavigate, useParams } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconBack, IconClose, IconPlay } from '../components/Icons.jsx';
import { useSession } from '../net/session';
import { asPerson, useSocial } from '../context/SocialContext.jsx';
import { MODES, SEATS } from '../game/constants';
import './pages.css';

const TEAM_MODES = ['2v2', 'side'];
const HINT = {
  '2v2': 'Đồng đội ngồi đối diện — bài của nhau luôn hiện ra với bạn',
  side: 'Đồng đội ngồi ngay bên trái bạn — hai người nhìn thấy bài của nhau',
};

/** Phòng tổ đội 2 vs 2 / Side to Side: mời 1 bạn làm đồng đội rồi đi ghép trận tìm đối thủ. */
export default function TeamRoom() {
  const { mode } = useParams();
  if (!TEAM_MODES.includes(mode)) return <Navigate to="/matchmaking/classic" replace />;
  return <Room key={mode} mode={mode} />;
}

function Room({ mode }) {
  const cfg = MODES[mode];
  const navigate = useNavigate();
  const { me, friends } = useSocial();
  const { party, request, receive, connection } = useSession();
  const [pending, setPending] = useState(false);
  const [toast, setToast] = useState(null);
  const teammateId = party?.members.find((member) => (member.userId ?? member) !== me.id);
  const teammate = teammateId && (typeof teammateId === 'object'
    ? asPerson(teammateId)
    : friends.find((friend) => friend.id === teammateId) ??
      asPerson({ userId: teammateId, name: `UNO#${teammateId}` }));
  const isLeader = !party?.members.length || party.leaderId === me.id;
  const connected = connection === 'connected';
  const notify = (text) => setToast({ id: Date.now(), text });
  const list = friends.filter((friend) => !friend.pending)
    .sort((a, b) => Number(b.online) - Number(a.online));
  const onlineCount = list.filter((friend) => friend.online).length;

  const command = async (type, payload) => {
    if (!connected || pending) return false;
    setPending(true);
    try {
      const response = await request(type, payload);
      if (type === 'QUEUE_JOIN') {
        if (response?.queue) receive('QUEUE_STATE', response.queue);
        navigate(`/matchmaking/${mode}`);
      } else if (type === 'INVITE_SEND') {
        notify('Đã gửi lời mời tổ đội');
      }
      return true;
    } catch (error) {
      notify(error.message);
      return false;
    } finally {
      setPending(false);
    }
  };
  const leave = () => command('PARTY_LEAVE');
  const back = async () => {
    if (!teammate || await leave()) navigate('/lobby');
  };

  const seatOf = (i) => {
    if (i === 0) return { kind: 'me', person: me };
    if (i === cfg.teammateSeat) return { kind: 'mate', person: teammate };
    return { kind: 'enemy', person: null };
  };

  const friendButton = (friend) => {
    if (!friend.online) return null;
    if (teammate?.id === friend.id) {
      return <button type="button" className="btn btn--sm btn--outline" disabled>Trong phòng</button>;
    }
    return <button type="button" className="btn btn--sm btn--ghost"
      disabled={!!teammate || !connected || pending}
      onClick={() => command('INVITE_SEND', { userId: friend.id, kind: 'party', mode })}>Mời</button>;
  };

  return (
    <div className="screen">
      <MenuBackground />

      <header className="page-head">
        <button type="button" className="icon-btn" onClick={back} aria-label="Quay lại">
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
                    <button type="button" className="seat-remove" onClick={leave} disabled={pending || !connected} aria-label="Rời tổ đội">
                      <IconClose size={14} />
                    </button>
                  )}
                  <Avatar person={person} size={56} />
                  <div className="room-seat__name">
                    {seat.kind === 'me' && `${person.name} (Bạn)`}
                    {seat.kind === 'mate' && (person ? person.name : 'Ghế đồng đội')}
                    {seat.kind === 'enemy' && 'Ghép trận sẽ tìm'}
                  </div>
                  {seat.kind === 'me' && isLeader && <span className="chip seat-chip seat-chip--host">Chủ phòng</span>}
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
            disabled={!isLeader || pending || !connected}
            onClick={() => command('QUEUE_JOIN', { mode })}
          >
            <IconPlay /> Tìm trận
          </button>
        </div>
      </div>

      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}
