import { useState } from 'react';
import { Navigate, useNavigate } from 'react-router-dom';
import Avatar from '../components/Avatar.jsx';
import MenuBackground from '../components/MenuBackground.jsx';
import { IconCards, IconCheck, IconCrown, IconMegaphone, IconPlay, IconPlus, IconSwords, IconTrophy } from '../components/Icons.jsx';
import { useSocial } from '../context/SocialContext.jsx';
import { RANKS } from '../data/mockData';
import { MODES } from '../game/constants';
import './pages.css';

const fmtTime = (sec) => `${String(Math.floor(sec / 60)).padStart(2, '0')}:${String(sec % 60).padStart(2, '0')}`;
const rankOf = (elo) => RANKS[Math.max(0, RANKS.findLastIndex((r) => elo >= r.min))];

/** Trang tổng kết sau mỗi ván (mọi chế độ). Dữ liệu lấy từ lastMatch trong SocialContext. */
export default function MatchResult() {
  const navigate = useNavigate();
  const { lastMatch: m, friendState, sendFriendRequest } = useSocial();
  const [toast, setToast] = useState(null);
  const notify = (text) => setToast({ id: Date.now(), text });

  if (!m) return <Navigate to="/lobby" replace />;

  const cfg = MODES[m.mode];
  const addFriend = (p) => {
    sendFriendRequest(p);
    notify(`Đã gửi lời mời kết bạn tới ${p.name}`);
  };

  const row = (p, rank) => (
    <PlayerRow key={p.id} p={p} rank={rank} relation={p.isHuman || p.isBot ? null : friendState(p.id)} onAdd={addFriend} />
  );

  const eloAfter = m.eloBefore + (m.eloDelta ?? 0);
  const tier = rankOf(eloAfter);

  return (
    <div className="screen">
      <MenuBackground />

      <div className={`mr ${m.won ? 'is-win' : 'is-lose'}`}>
        <div className="mr__burst" aria-hidden />

        <div className="mr__head">
          <div className="mr__title display">{m.won ? 'CHIẾN THẮNG!' : 'THUA RỒI!'}</div>
          <div className="mr__sub">
            {m.teams ? (m.won ? 'Đội bạn thắng' : 'Đội đối thủ thắng') : m.won ? 'Bạn về nhất' : `${winnerName(m)} về nhất`}
            <span className="mr__sep" />
            <span className="dot" style={{ background: cfg.accent }} /> {cfg.label}
            <span className="mr__sep" />
            {fmtTime(m.durationSec)}
          </div>
        </div>

        {m.ranked ? (
          <div className="panel mr__elo">
            <IconTrophy size={22} />
            <span className="display mr__elo-num">{m.eloBefore.toLocaleString('vi-VN')}</span>
            <span className="muted">→</span>
            <span className="display mr__elo-num">{eloAfter.toLocaleString('vi-VN')}</span>
            <span className={`display mr__delta ${m.eloDelta >= 0 ? 'is-up' : 'is-down'}`}>
              {m.eloDelta >= 0 ? `+${m.eloDelta}` : m.eloDelta}
            </span>
            <span className="chip" style={{ color: tier.color }}>{tier.name}</span>
          </div>
        ) : (
          <div className="chip mr__unranked">Không tính ELO</div>
        )}

        {m.teams ? (
          <div className="mr__teams">
            {[m.myTeam, ...new Set(m.players.map((p) => p.team).filter((t) => t !== m.myTeam))].map((team) => (
              <section key={team} className={`panel mr__team ${team === m.winnerTeam ? 'is-winner' : ''}`}>
                <div className="mr__team-head">
                  <span className="section-title">{team === m.myTeam ? 'ĐỘI CỦA BẠN' : 'ĐỘI ĐỐI THỦ'}</span>
                  {team === m.winnerTeam && <span className="chip mr__win-chip"><IconTrophy size={13} /> Thắng</span>}
                </div>
                {m.players.filter((p) => p.team === team).map((p) => row(p))}
              </section>
            ))}
          </div>
        ) : (
          <section className="panel mr__solo">
            {[...m.players].sort((a, b) => a.points - b.points || a.cardsLeft - b.cardsLeft).map((p, i) => row(p, i + 1))}
          </section>
        )}

        <div className="mr__actions">
          <button type="button" className="btn btn--outline" onClick={() => navigate('/lobby')}>Về sảnh</button>
          {m.ranked && m.teams && (
            <button type="button" className="btn btn--ghost" onClick={() => navigate(`/room/${m.mode}`)}>Về phòng</button>
          )}
          <button
            type="button"
            className="btn btn--primary"
            onClick={() => navigate(m.ranked ? `/matchmaking/${m.mode}` : '/custom')}
          >
            <IconPlay /> Chơi tiếp
          </button>
        </div>
      </div>

      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}

const winnerName = (m) => m.players.find((p) => p.id === m.winnerId)?.name;

function PlayerRow({ p, rank, relation, onAdd }) {
  const { played = 0, attacks = 0, unoCalls = 0 } = p.stats ?? {};
  return (
    <div className={`mr-row ${p.isWinner ? 'is-winner' : ''} ${p.isHuman ? 'is-me' : ''}`}>
      {rank != null && <span className="mr-row__rank display">{rank}</span>}
      <Avatar person={p} size={44} shape="square" />
      <div className="mr-row__main">
        <div className="mr-row__name">
          {p.isHuman ? `${p.name} (Bạn)` : p.name}
          {p.isWinner && <span className="mr-row__crown" title="Hết bài trước"><IconCrown size={16} /></span>}
        </div>
        <div className="mr-row__stats">
          <span><IconCards size={14} /> {played} lá đã đánh</span>
          <span><IconSwords size={14} /> {attacks} lần +2/+4</span>
          <span><IconMegaphone size={14} /> {unoCalls} UNO</span>
        </div>
      </div>
      <div className="mr-row__score">
        <b className="display">{p.cardsLeft ? `-${p.points}` : '+0'}</b>
        <span>{p.cardsLeft} lá còn lại</span>
      </div>
      <div className="mr-row__friend">
        {relation === 'friend' && <span className="chip mr-row__tag">Bạn bè</span>}
        {relation === 'pending' && (
          <button type="button" className="btn btn--sm btn--outline" disabled>
            <IconCheck /> Đã gửi
          </button>
        )}
        {relation === 'none' && (
          <button type="button" className="btn btn--sm btn--ghost" onClick={() => onAdd(p)}>
            <IconPlus size={14} /> Kết bạn
          </button>
        )}
      </div>
    </div>
  );
}
