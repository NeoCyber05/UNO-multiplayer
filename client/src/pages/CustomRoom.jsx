import { useEffect, useState } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import Modal from '../components/Modal.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconBack, IconBot, IconClose, IconCopy } from '../components/Icons.jsx';
import { useSocial } from '../context/SocialContext.jsx';
import { BOT_LEVELS } from '../data/mockData';
import './pages.css';

const ROOM_CODE = 'B7K2';

// teams[i] = đội của ghế i ('A' = đội bạn). Khớp teammateSeat trong game/constants.js.
const RULES = [
  { key: 'classic', label: 'Classic', sub: 'Mỗi người một mình', teams: null },
  { key: '2v2', label: '2 vs 2', sub: 'Đồng đội ngồi đối diện', teams: ['A', 'B', 'A', 'B'] },
  { key: 'side', label: 'Side to Side', sub: 'Đồng đội ngồi cạnh (bên trái)', teams: ['A', 'A', 'B', 'B'] },
];
const SEAT_POS = ['bottom', 'left', 'top', 'right'];

const levelOf = (key) => BOT_LEVELS.find((l) => l.key === key) ?? BOT_LEVELS[1];
const newBot = () => ({ kind: 'bot', level: 'normal' });
const botPerson = (i, levelKey) => {
  const lv = levelOf(levelKey);
  return { id: `bot-${i}`, name: `Bot ${i}`, initials: 'BOT', tint: lv.tint, isBot: true, botLevel: lv.key };
};

export default function CustomRoom() {
  const navigate = useNavigate();
  const location = useLocation();
  const { me, friends } = useSocial();
  const friendList = friends.filter((f) => !f.pending);
  const [ruleKey, setRuleKey] = useState('classic');
  const [seats, setSeats] = useState([{ kind: 'me', person: me }, null, null, null]);
  const [pickSeat, setPickSeat] = useState(null);
  const [joinOpen, setJoinOpen] = useState(false);
  const [joinCode, setJoinCode] = useState('');
  const [toast, setToast] = useState(null);

  const rule = RULES.find((r) => r.key === ruleKey);
  const filled = seats.filter(Boolean).length;
  const notify = (text) => setToast({ id: Date.now(), text });
  const inRoom = (f) => seats.some((s) => s?.person?.id === f.id);
  const placeAt = (i, occ) => setSeats((s) => s.map((x, k) => (k === i ? occ : x)));

  const clearSeat = (i) => {
    placeAt(i, null);
    if (pickSeat === i) setPickSeat(null);
  };

  const invite = (f) => {
    const target = pickSeat != null && !seats[pickSeat] ? pickSeat : seats.findIndex((s) => !s);
    if (target < 0) return notify('Phòng đã đủ người');
    placeAt(target, { kind: 'friend', person: f });
    setPickSeat(null);
    notify(`${f.name} đã vào phòng`);
  };

  useEffect(() => {
    const friendId = location.state?.inviteFriendId;
    if (!friendId) return;
    const friend = friendList.find((f) => f.id === friendId);
    if (!friend) return;
    setSeats((s) => {
      if (s.some((occ) => occ?.person?.id === friend.id)) return s;
      const target = s.findIndex((occ) => !occ);
      if (target < 0) return s;
      return s.map((occ, i) => (i === target ? { kind: 'friend', person: friend } : occ));
    });
    notify(`${friend.name} đã vào phòng`);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const start = () => {
    const lineup = seats.map((occ, i) => (occ.kind === 'bot' ? botPerson(i, occ.level) : occ.person));
    navigate(`/game/${ruleKey}`, { state: { lineup, ranked: false } });
  };

  const fillBots = () => {
    setSeats((s) => s.map((x) => x ?? newBot()));
    setPickSeat(null);
  };

  const copyCode = async () => {
    try {
      await navigator.clipboard.writeText(ROOM_CODE);
      notify('Đã sao chép mã phòng');
    } catch {
      notify(`Mã phòng: ${ROOM_CODE}`);
    }
  };

  const submitJoin = (e) => {
    e.preventDefault();
    if (joinCode.trim().length < 4) return notify('Mã phòng gồm 4 ký tự');
    setJoinOpen(false);
    notify(`Không tìm thấy phòng ${joinCode}`);
    setJoinCode('');
  };

  return (
    <div className="screen">
      <MenuBackground />

      <header className="page-head">
        <button type="button" className="icon-btn" onClick={() => navigate('/lobby')} aria-label="Quay lại">
          <IconBack />
        </button>
        <div style={{ flexGrow: 1 }}>
          <div className="display page-head__title">Phòng tùy chỉnh</div>
          <div className="muted" style={{ fontSize: 13, marginTop: 2 }}>Mời bạn bè hoặc thêm bot · không tính ELO</div>
        </div>
        <button type="button" className="btn btn--ghost" onClick={() => setJoinOpen(true)}>Nhập mã phòng</button>
        <button type="button" className="room-code" onClick={copyCode} aria-label="Sao chép mã phòng">
          <span className="muted" style={{ fontSize: 11 }}>MÃ PHÒNG</span>
          <b className="display">{ROOM_CODE}</b>
          <IconCopy />
        </button>
      </header>

      <div className="room">
        <div className="room__table-wrap">
          <div className="rule-tabs" role="tablist" aria-label="Luật chơi">
            {RULES.map((r) => (
              <button
                key={r.key}
                type="button"
                role="tab"
                aria-selected={r.key === ruleKey}
                className={`rule-tab ${r.key === ruleKey ? 'is-on' : ''}`}
                onClick={() => setRuleKey(r.key)}
              >
                <b>{r.label}</b>
                <span>{r.sub}</span>
              </button>
            ))}
          </div>

          <div className="room-table">
            <div className="room-table__felt"><span className="display">UNO</span></div>
            {SEAT_POS.map((pos, i) => {
              const occ = seats[i];
              const team = rule.teams?.[i];
              const person = occ?.kind === 'bot' ? botPerson(i, occ.level) : occ?.person ?? null;
              const rel = team ? (team === 'A' ? 'is-team' : 'is-enemy') : '';
              return (
                <div
                  key={pos}
                  className={`room-seat room-seat--${pos} ${occ ? '' : 'is-empty'} ${rel} ${pickSeat === i ? 'is-picking' : ''}`}
                >
                  {occ && occ.kind !== 'me' && (
                    <button type="button" className="seat-remove" onClick={() => clearSeat(i)} aria-label="Bỏ khỏi ghế">
                      <IconClose size={14} />
                    </button>
                  )}
                  <Avatar person={person} size={56} />
                  <div className="room-seat__name">
                    {!occ ? 'Ô trống' : occ.kind === 'me' ? `${person.name} (Bạn)` : person.name}
                  </div>
                  {occ?.kind === 'me' && <span className="chip seat-chip seat-chip--host">Chủ phòng</span>}
                  {occ && occ.kind !== 'me' && team && (
                    <span className={`chip seat-chip ${team === 'A' ? 'seat-chip--team' : 'seat-chip--enemy'}`}>
                      {team === 'A' ? 'Đồng đội' : 'Đối thủ'}
                    </span>
                  )}
                  {occ?.kind === 'bot' && (
                    <select
                      className="seat-level"
                      style={{ '--c': levelOf(occ.level).tint }}
                      value={occ.level}
                      onChange={(e) => placeAt(i, { ...occ, level: e.target.value })}
                      aria-label={`Độ khó Bot ${i}`}
                    >
                      {BOT_LEVELS.map((l) => (
                        <option key={l.key} value={l.key}>{l.label}</option>
                      ))}
                    </select>
                  )}
                  {!occ && (
                    <div className="seat-actions">
                      <button type="button" className="btn btn--sm btn--ghost" onClick={() => setPickSeat(i)}>Mời bạn</button>
                      <button type="button" className="btn btn--sm btn--primary" onClick={() => placeAt(i, newBot())}>+ Bot</button>
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
                      disabled={!f.online || here}
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

          <button type="button" className="btn btn--outline btn--block" disabled={filled === 4} onClick={fillBots}>
            <IconBot size={18} /> Thêm bot vào mọi ô trống
          </button>
          <button
            type="button"
            className="btn btn--primary btn--block"
            disabled={filled < 4}
            onClick={start}
            title={filled < 4 ? 'Cần đủ 4 ghế' : undefined}
          >
            {filled < 4 ? `Bắt đầu (${filled}/4)` : 'Bắt đầu'}
          </button>
        </div>
      </div>

      <Modal open={joinOpen} onClose={() => setJoinOpen(false)} label="Nhập mã phòng">
        <form className="join-form" onSubmit={submitJoin}>
          <div className="display" style={{ fontSize: 22, fontWeight: 600 }}>Nhập mã phòng</div>
          <input
            className="input join-form__input"
            placeholder="VD: B7K2"
            maxLength={4}
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
