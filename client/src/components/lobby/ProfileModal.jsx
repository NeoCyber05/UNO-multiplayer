import { useState } from 'react';
import Modal from '../Modal.jsx';
import Avatar from '../Avatar.jsx';
import {
  IconCards,
  IconCheck,
  IconClose,
  IconCopy,
  IconDiamond,
  IconFlame,
  IconHandshake,
  IconMegaphone,
  IconTrophy,
  IconUser,
} from '../Icons.jsx';
import { MATCH_HISTORY, PROFILE, RANKS } from '../../data/mockData';

const TABS = [
  { key: 'overview', label: 'Tổng quan' },
  { key: 'history', label: 'Lịch sử' },
  { key: 'badges', label: 'Thành tích' },
];

const pct = (a, b) => (b ? Math.round((a / b) * 100) : 0);

/** Bậc hiện tại + bậc kế tiếp + % tiến độ lên bậc. */
function rankOf(elo) {
  const i = RANKS.findLastIndex((r) => elo >= r.min);
  const cur = RANKS[i];
  const next = RANKS[i + 1];
  const progress = next ? pct(elo - cur.min, next.min - cur.min) : 100;
  return { cur, next, progress };
}

function Overview() {
  const { stats, modes } = PROFILE;
  const tiles = [
    { label: 'Số trận', value: stats.matches },
    { label: 'Thắng', value: stats.wins },
    { label: 'Tỉ lệ thắng', value: `${pct(stats.wins, stats.matches)}%` },
    { label: 'Chuỗi thắng', value: stats.streak, sub: `Cao nhất ${stats.bestStreak}` },
  ];
  return (
    <>
      <div className="pf-tiles">
        {tiles.map((t) => (
          <div key={t.label} className="pf-tile">
            <span className="pf-tile__value display">{t.value}</span>
            <span className="pf-tile__label">{t.label}</span>
            {t.sub && <span className="pf-tile__sub">{t.sub}</span>}
          </div>
        ))}
      </div>

      <h3 className="pf-section display">Theo chế độ</h3>
      <div className="pf-modes">
        {modes.map((m) => {
          const rate = pct(m.wins, m.matches);
          return (
            <div key={m.key} className="pf-mode">
              <span className="pf-mode__name">{m.title}</span>
              <span className="pf-mode__bar"><i style={{ width: `${rate}%` }} /></span>
              <span className="pf-mode__rate display">{rate}%</span>
              <span className="pf-mode__count">{m.wins}/{m.matches} trận</span>
            </div>
          );
        })}
      </div>

      <div className="pf-fun">
        <span><IconMegaphone size={16} /> Hô UNO <b>{stats.unoCalls}</b> lần</span>
        <span><IconCards size={16} /> Ném <b>{stats.plus4}</b> lá +4</span>
      </div>
    </>
  );
}

function History() {
  return (
    <ol className="pf-history">
      {MATCH_HISTORY.map((m) => (
        <li key={m.id} className={`pf-match ${m.place === 1 ? 'is-win' : ''}`}>
          <span className="pf-match__place display">#{m.place}</span>
          <span className="pf-match__info">
            <b>{m.mode}</b>
            <small>{m.players} người · {m.when}</small>
          </span>
          <span className="pf-match__result">{m.place === 1 ? 'THẮNG' : 'THUA'}</span>
          <span className={`pf-match__delta display ${m.delta > 0 ? 'is-up' : m.delta < 0 ? 'is-down' : ''}`}>
            {m.delta == null ? '—' : `${m.delta > 0 ? '+' : ''}${m.delta}`}
          </span>
        </li>
      ))}
    </ol>
  );
}

// PROFILE.badges[].icon -> [icon, màu]
const BADGE_ICONS = {
  trophy: [IconTrophy, '#ffc94a'],
  flame: [IconFlame, '#ff8a3d'],
  cards: [IconCards, '#4c86ff'],
  megaphone: [IconMegaphone, '#ff5262'],
  handshake: [IconHandshake, '#35d399'],
  diamond: [IconDiamond, '#7fd8ff'],
};

function Badges() {
  return (
    <div className="pf-badges">
      {PROFILE.badges.map((b) => {
        const [Icon, color] = BADGE_ICONS[b.icon];
        return (
          <div key={b.key} className={`pf-badge ${b.got ? '' : 'is-locked'}`}>
            <span className="pf-badge__icon" style={{ '--c': color }}>
              <Icon size={28} />
            </span>
            <span className="pf-badge__name">{b.name}</span>
          </div>
        );
      })}
    </div>
  );
}

export default function ProfileModal({ open, onClose, me }) {
  const [tab, setTab] = useState('overview');
  const [copied, setCopied] = useState(false);
  const { cur, next, progress } = rankOf(me.elo);

  const copyTag = () => {
    navigator.clipboard?.writeText(PROFILE.tag);
    setCopied(true);
    setTimeout(() => setCopied(false), 1500);
  };

  return (
    <Modal open={open} onClose={onClose} label="Hồ sơ" className="lobby-modal profile">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconUser size={24} /> Hồ sơ</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>

      <div className="profile__body">
        <aside className="pf-card" style={{ '--rank': cur.color }}>
          <Avatar person={me} size={112} shape="square" className="pf-card__avatar" />
          <div className="pf-card__name display">{me.name}</div>
          <button type="button" className="pf-card__tag" onClick={copyTag} title="Sao chép ID">
            {PROFILE.tag} {copied ? <IconCheck /> : <IconCopy size={13} />}
          </button>
          <span className="chip pf-card__title">{PROFILE.title}</span>

          <div className="pf-rank">
            <span className="pf-rank__emblem display"><IconTrophy size={18} /> {cur.name}</span>
            <span className="pf-rank__elo display">{me.elo.toLocaleString('vi-VN')} ELO</span>
            <span className="pf-bar pf-bar--rank"><i style={{ width: `${progress}%` }} /></span>
            <small>{next ? `Còn ${next.min - me.elo} ELO lên ${next.name}` : 'Bậc cao nhất'}</small>
          </div>

          <div className="pf-level">
            <span className="pf-level__row"><b>Cấp {me.level}</b><span>{me.xp}% XP</span></span>
            <span className="pf-bar pf-bar--xp"><i style={{ width: `${me.xp}%` }} /></span>
          </div>

          <div className="pf-card__foot">
            <span><IconFlame size={15} /> Chuỗi {PROFILE.stats.streak} trận thắng</span>
            <span>Tham gia {PROFILE.joined}</span>
          </div>
        </aside>

        <section className="profile__main">
          <div className="seg" role="tablist">
            {TABS.map((t) => (
              <button
                key={t.key}
                type="button"
                role="tab"
                aria-selected={tab === t.key}
                className={`seg__btn ${tab === t.key ? 'is-on' : ''}`}
                onClick={() => setTab(t.key)}
              >
                {t.label}
              </button>
            ))}
          </div>
          <div key={tab} className="profile__panel">
            {tab === 'overview' && <Overview />}
            {tab === 'history' && <History />}
            {tab === 'badges' && <Badges />}
          </div>
        </section>
      </div>
    </Modal>
  );
}
