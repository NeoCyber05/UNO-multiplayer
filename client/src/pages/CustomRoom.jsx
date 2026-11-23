import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import Modal from '../components/Modal.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconBack, IconBot, IconCheck, IconClose, IconCopy } from '../components/Icons.jsx';
import { useSession } from '../net/session';
import { asPerson, useSocial } from '../context/SocialContext.jsx';
import { BOT_LEVELS } from '../data/mockData';
import { LOBBY_MODES } from '../data/lobbyModes';
import './pages.css';

// Đội được xác định theo ghế tuyệt đối trước khi xoay bàn về phía người xem.
const RULES = [
  { key: 'classic', label: 'Classic', teams: null },
  { key: '2v2', label: '2 vs 2', teams: ['A', 'B', 'A', 'B'] },
  { key: 'side', label: 'Side to Side', teams: ['A', 'A', 'B', 'B'] },
  { key: 'draft', label: 'UNO Draft', teams: null },
];
const SEAT_POS = ['bottom', 'left', 'top', 'right'];

const levelOf = (key) => BOT_LEVELS.find((l) => l.key === key) ?? BOT_LEVELS[1];

export default function CustomRoom() {
  const navigate = useNavigate();
  const { me, friends } = useSocial();
  const { room: roomSnapshot, request, connection } = useSession();
  const friendList = friends.filter((friend) => !friend.pending);
  const [draftMode, setDraftMode] = useState('classic');
  const [pickSeat, setPickSeat] = useState(null);
  const [joinOpen, setJoinOpen] = useState(false);
  const [joinCode, setJoinCode] = useState('');
  const [toast, setToast] = useState(null);
  const [pending, setPending] = useState(false);

  const mySeatEntry = roomSnapshot?.seats.find((seat) => seat?.userId === me.id);
  const room = mySeatEntry ? roomSnapshot : null;
  const roomCode = room?.code;
  const ruleKey = room?.mode ?? draftMode;
  const rule = RULES.find((item) => item.key === ruleKey);
  const mySeat = mySeatEntry?.seat;
  const isHost = !!room && room.hostSeat === mySeat;
  const available = connection === 'connected' && !pending;
  const seats = SEAT_POS.map((_, index) => room ? room.seats[(index + mySeat) % SEAT_POS.length] : null);
  const filled = seats.filter(Boolean).length;
  const notify = (text) => setToast({ id: Date.now(), text });
  const inRoom = (friend) => seats.some((seat) => seat?.userId === friend.id);

  const run = async (operation) => {
    if (!available) return false;
    setPending(true);
    try {
      await operation();
      return true;
    } catch (error) {
      notify(error.message);
      return false;
    } finally {
      setPending(false);
    }
  };
  const command = (type, payload) => run(() => request(type, payload));
  const chooseMode = (mode) => room ? command('ROOM_MODE', { mode }) : setDraftMode(mode);
  const back = async () => {
    if (!room || await command('ROOM_LEAVE')) navigate('/lobby');
  };
  const invite = async (friend) => {
    if (await command('INVITE_SEND', { userId: friend.id, kind: 'room' })) {
      setPickSeat(null);
      notify(`Đã gửi lời mời tới ${friend.name}`);
    }
  };
  const fillBots = () => run(async () => {
    for (let index = 0; index < seats.length; index++) {
      if (!seats[index]) await request('ROOM_BOT', { seat: (index + mySeat) % seats.length, level: 'normal' });
    }
    setPickSeat(null);
  });
  const copyCode = async () => {
    try {
      await navigator.clipboard.writeText(roomCode);
      notify('Đã sao chép mã phòng');
    } catch {
      notify(`Mã phòng: ${roomCode}`);
    }
  };
  const submitJoin = async (event) => {
    event.preventDefault();
    const code = joinCode.trim();
    if (!/^[A-Z0-9]{6}$/.test(code)) return notify('Mã phòng cần 6 ký tự A–Z hoặc 0–9');
    if (await command('ROOM_JOIN', { code })) {
      setJoinOpen(false);
      setJoinCode('');
    }
  };

  return (
    <div className="screen custom-room">
      <MenuBackground />

      <header className="page-head">
        <button type="button" className="icon-btn" onClick={back} aria-label="Quay lại">
          <IconBack />
        </button>
        <div style={{ flexGrow: 1 }}>
          <div className="display page-head__title">Phòng tùy chỉnh</div>
          <div className="muted" style={{ fontSize: 13, marginTop: 2 }}>Mời bạn bè hoặc thêm bot · không tính ELO</div>
        </div>
        {!room && <button type="button" className="btn btn--ghost" disabled={!available} onClick={() => setJoinOpen(true)}>Nhập mã phòng</button>}
        {roomCode && <button type="button" className="room-code" onClick={copyCode} aria-label="Sao chép mã phòng">
          <span className="room-code__label">MÃ PHÒNG</span>
          <b className="display">{roomCode}</b>
          <IconCopy />
        </button>}
      </header>

      <div className="room">
        <div className="room__table-wrap">
          <div className="rule-tabs" role="group" aria-label="Luật chơi">
            {RULES.map((r) => (
              <button
                key={r.key}
                type="button"
                aria-pressed={r.key === ruleKey}
                className={`rule-tab ${r.key === ruleKey ? 'is-on' : ''}`}
                disabled={!available || (!!room && !isHost)}
                onClick={() => chooseMode(r.key)}
              >
                <img className="rule-tab__art" src={LOBBY_MODES.find((m) => m.key === r.key)?.image} alt="" />
                <b>{r.label}<span className="rule-tab__check" aria-hidden="true"><IconCheck /></span></b>
              </button>
            ))}
          </div>

          <div className="room-table">
            <div className="room-table__felt"><span className="display">UNO</span></div>
            {SEAT_POS.map((pos, i) => {
              const occ = seats[i];
              const absoluteSeat = room ? (i + mySeat) % seats.length : null;
              const team = room ? rule.teams?.[absoluteSeat] : null;
              const isMe = occ?.userId === me.id;
              const mine = team === rule.teams?.[mySeat];
              const person = occ ? asPerson(occ) : null;
              const rel = team ? (mine ? 'is-team' : 'is-enemy') : '';
              return (
                <div
                  key={pos}
                  className={`room-seat room-seat--${pos} ${occ ? '' : 'is-empty'} ${rel} ${pickSeat === i ? 'is-picking' : ''}`}
                >
                  {occ && !isMe && isHost && (
                    <button type="button" className="seat-remove" disabled={!available} onClick={() => command('ROOM_KICK', { seat: absoluteSeat })} aria-label="Bỏ khỏi ghế">
                      <IconClose size={14} />
                    </button>
                  )}
                  <Avatar person={person} size={56} />
                  <div className="room-seat__name">
                    {!occ ? 'Ô trống' : isMe ? `${person.name} (Bạn)` : person.name}
                  </div>
                  {occ && absoluteSeat === room?.hostSeat && <span className="chip seat-chip seat-chip--host">Chủ phòng</span>}
                  {occ && !isMe && team && (
                    <span className={`chip seat-chip ${mine ? 'seat-chip--team' : 'seat-chip--enemy'}`}>
                      {mine ? 'Đồng đội' : 'Đối thủ'}
                    </span>
                  )}
                  {occ?.isBot && (
                    <select
                      className="seat-level"
                      style={{ '--c': levelOf(occ.botLevel).tint }}
                      value={occ.botLevel}
                      disabled={!isHost || !available}
                      onChange={(e) => command('ROOM_BOT', { seat: absoluteSeat, level: e.target.value })}
                      aria-label={`Độ khó Bot ${i}`}
                    >
                      {BOT_LEVELS.map((l) => (
                        <option key={l.key} value={l.key}>{l.label}</option>
                      ))}
                    </select>
                  )}
                  {!occ && isHost && (
                    <div className="seat-actions">
                      <button type="button" className="btn btn--sm btn--ghost" onClick={() => setPickSeat(i)}>Mời bạn</button>
                      <button type="button" className="btn btn--sm btn--primary" disabled={!available} onClick={() => command('ROOM_BOT', { seat: absoluteSeat, level: 'normal' })}>+ Bot</button>
                    </div>
                  )}
                </div>
              );
            })}
          </div>

          {rule.teams && (
            <div className="room__legend">
              <span><i className="dot" style={{ background: 'var(--green)' }} /> Đội của bạn</span>
              <span><i className="dot" style={{ background: 'var(--red)' }} /> Đội đối thủ</span>
            </div>
          )}
        </div>

        <div className="room__side">
          <div className={`panel room__box custom-friends ${pickSeat != null ? 'is-picking' : ''}`}>
            <div className="section-title">
              {pickSeat != null ? 'CHỌN BẠN ĐỂ MỜI VÀO GHẾ TRỐNG' : `BẠN BÈ · ${friendList.filter((f) => f.online).length} ONLINE`}
            </div>
            <div className="friend-list">
              {friendList.map((f) => {
                const here = inRoom(f);
                return (
                  <div key={f.id} className={`friend ${f.online ? '' : 'is-offline'}`}>
                    <div className="friend__avatar">
                      <Avatar person={f} size={40} />
                      <span className={`friend__status ${f.online ? 'is-on' : ''}`} />
                    </div>
                    <div style={{ flexGrow: 1, minWidth: 0 }}>
                      <div style={{ fontWeight: 700, fontSize: 14 }}>{f.name}</div>
                      <div className="muted friend__sub">{f.status}</div>
                    </div>
                    <button
                      type="button"
                      className={`btn btn--sm ${here ? 'btn--outline' : 'btn--ghost'}`}
                      disabled={!room || !available || !f.online || here || filled === 4}
                      onClick={() => invite(f)}
                    >
                      {here ? 'Trong phòng' : 'Mời'}
                    </button>
                  </div>
                );
              })}
            </div>
          </div>

          <div style={{ flexGrow: 1 }} />

          {!room && <button type="button" className="btn btn--primary btn--block"
            disabled={!available} onClick={() => command('ROOM_CREATE', { mode: ruleKey })}>Tạo phòng</button>}
          {isHost && <><button type="button" className="btn btn--outline btn--block" disabled={!available || filled === 4} onClick={fillBots}>
            <IconBot size={18} /> Thêm bot vào mọi ô trống
          </button>
          <button
            type="button"
            className="btn btn--primary btn--block"
            disabled={!available || filled < 4}
            onClick={() => command('ROOM_START')}
            title={filled < 4 ? 'Cần đủ 4 ghế' : undefined}
          >
            {filled < 4 ? `Bắt đầu (${filled}/4)` : 'Bắt đầu'}
          </button></>}
        </div>
      </div>

      <Modal open={joinOpen} onClose={() => setJoinOpen(false)} label="Nhập mã phòng">
        <form className="join-form" onSubmit={submitJoin}>
          <div className="display" style={{ fontSize: 22, fontWeight: 600 }}>Nhập mã phòng</div>
          <input
            className="input join-form__input"
            placeholder="VD: B7K2X9"
            maxLength={6}
            minLength={6}
            autoFocus
            value={joinCode}
            onChange={(e) => setJoinCode(e.target.value.toUpperCase())}
            aria-label="Mã phòng"
          />
          <div style={{ display: 'flex', gap: 12 }}>
            <button type="button" className="btn btn--outline" style={{ flex: 1 }} onClick={() => setJoinOpen(false)}>Huỷ</button>
            <button type="submit" className="btn btn--primary" style={{ flex: 1 }}>Vào phòng</button>
          </div>
        </form>
      </Modal>

      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}
