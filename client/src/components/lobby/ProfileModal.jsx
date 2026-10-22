import { useState } from 'react';
import Modal from '../Modal.jsx';
import Avatar from '../Avatar.jsx';
import {
  IconCards,
  IconCheck,
  IconClose,
  IconCopy,
  IconCrown,
  IconDiamond,
  IconEdit,
  IconFlame,
  IconHandshake,
  IconMegaphone,
  IconShield,
  IconTrophy,
  IconUser,
} from '../Icons.jsx';
import { MATCH_HISTORY, PROFILE, RANKS } from '../../data/mockData';
import RankBadge from '../RankBadge.jsx';
import { AVATARS } from '../../data/avatars.jsx';
import { useSocial } from '../../context/SocialContext.jsx';

const TABS = [
  { key: 'overview', label: 'Tổng quan' },
  { key: 'history', label: 'Lịch sử' },
  { key: 'badges', label: 'Thành tựu' },
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

// PROFILE.badges[].icon -> icon; tier -> [sáng, tối] cho huy chương
const BADGE_ICONS = {
  trophy: IconTrophy,
  flame: IconFlame,
  cards: IconCards,
  megaphone: IconMegaphone,
  handshake: IconHandshake,
  diamond: IconDiamond,
  crown: IconCrown,
  shield: IconShield,
};
const TIERS = {
  bronze: { label: 'Đồng', c: ['#f2b27c', '#a9602f'] },
  silver: { label: 'Bạc', c: ['#eef2f6', '#8795a6'] },
  gold: { label: 'Vàng', c: ['#ffe27a', '#d9941a'] },
  diamond: { label: 'Kim cương', c: ['#b8f0ff', '#3a9fd6'] },
};

/** Huy chương lục giác SVG, icon ở giữa. */
function Medal({ icon, tier }) {
  const Icon = BADGE_ICONS[icon];
  const [hi, lo] = TIERS[tier].c;
  const id = `medal-${icon}`;
  return (
    <span className="pf-medal">
      <svg viewBox="0 0 80 88" width="72" height="79" aria-hidden="true">
        <defs>
          <linearGradient id={id} x1="0" y1="0" x2="1" y2="1">
            <stop offset="0" stopColor={hi} />
            <stop offset="1" stopColor={lo} />
          </linearGradient>
        </defs>
        <path d="M40 4l32 18v38L40 78 8 60V22z" fill={`url(#${id})`} />
        <path d="M40 12l25 14v30L40 70 15 56V26z" fill="#fff" fillOpacity=".22" stroke="#fff" strokeOpacity=".6" strokeWidth="1.5" />
        <path d="M20 82l20-8 20 8" fill="none" stroke={lo} strokeWidth="3" strokeLinecap="round" />
      </svg>
      <span className="pf-medal__icon"><Icon size={26} /></span>
    </span>
  );
}

function Badges() {
  const list = PROFILE.badges;
  const done = list.filter((b) => b.progress >= b.goal).length;
  return (
    <>
      <div className="pf-ach-sum">
        <span className="pf-ach-sum__text"><b className="display">{done}/{list.length}</b> thành tựu đã mở khoá</span>
        <span className="pf-bar pf-bar--xp"><i style={{ width: `${pct(done, list.length)}%` }} /></span>
      </div>
      <div className="pf-badges">
        {list.map((b) => {
          const got = b.progress >= b.goal;
          return (
            <div key={b.key} className={`pf-badge pf-badge--${b.tier} ${got ? '' : 'is-locked'}`}>
              <Medal icon={b.icon} tier={b.tier} />
              <span className="pf-badge__body">
                <span className="pf-badge__name">{b.name}</span>
                <span className="pf-badge__desc">{b.desc}</span>
                <span className="pf-bar pf-badge__bar"><i style={{ width: `${pct(Math.min(b.progress, b.goal), b.goal)}%` }} /></span>
                <span className="pf-badge__meta">
                  <span>{got ? 'Hoàn thành' : `${b.progress.toLocaleString('vi-VN')}/${b.goal.toLocaleString('vi-VN')}`}</span>
                  <span className="pf-badge__tier">{TIERS[b.tier].label}</span>
                </span>
              </span>
            </div>
          );
        })}
      </div>
    </>
  );
}

export default function ProfileModal({ open, onClose, me }) {
  const [tab, setTab] = useState('overview');
  const [copied, setCopied] = useState(false);
  const { cur, next, progress } = rankOf(me.elo);
  const { updateProfile } = useSocial();
  const [editing, setEditing] = useState(false);
  const [name, setName] = useState(me.name);
  const [picking, setPicking] = useState(false);

  const startEdit = () => {
    setName(me.name);
    setEditing(true);
  };
  const saveName = () => {
    updateProfile({ name });
    setEditing(false);
  };
  const pickAvatar = (key) => {
    updateProfile({ avatar: key });
    setPicking(false);
  };
  // Escape chỉ đóng popup/ô sửa, không đóng cả modal hồ sơ
  const escOnly = (close) => (e) => {
    if (e.key !== 'Escape') return;
    e.stopPropagation();
    close();
  };

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
          <div className="pf-av">
            <button
              type="button"
              className="pf-av__btn"
              onClick={() => setPicking((p) => !p)}
              aria-label="Đổi avatar"
              aria-expanded={picking}
              title="Đổi avatar"
            >
              <Avatar person={me} size={112} shape="square" className="pf-card__avatar" />
              <span className="pf-av__hint"><IconEdit size={14} /></span>
            </button>
            {picking && (
              <div className="pf-picker" role="radiogroup" aria-label="Chọn avatar" onKeyDown={escOnly(() => setPicking(false))}>
                {AVATARS.map((a) => (
                  <button
                    key={a.key}
                    type="button"
                    role="radio"
                    aria-checked={me.avatar === a.key}
                    className={`pf-edit__av ${me.avatar === a.key ? 'is-on' : ''}`}
                    onClick={() => pickAvatar(a.key)}
                  >
                    <Avatar person={{ name: a.key, avatar: a.key }} size={48} shape="square" />
                  </button>
                ))}
              </div>
            )}
          </div>
          {editing ? (
            <input
              className="pf-edit__name display"
              value={name}
              maxLength={16}
              autoFocus
              onChange={(e) => setName(e.target.value)}
              onBlur={saveName}
              onKeyDown={(e) => {
                if (e.key === 'Enter') saveName();
                escOnly(() => setEditing(false))(e);
              }}
              aria-label="Tên hiển thị"
            />
          ) : (
            <button type="button" className="pf-card__name display" onClick={startEdit} title="Đổi tên">
              {me.name} <IconEdit size={14} />
            </button>
          )}
          <button type="button" className="pf-card__tag" onClick={copyTag} title="Sao chép ID">
            {PROFILE.tag} {copied ? <IconCheck /> : <IconCopy size={13} />}
          </button>

          <div className="pf-rank">
            <span className="pf-rank__emblem display"><RankBadge elo={me.elo} size={48} label /></span>
            <span className="pf-rank__elo display">{me.elo.toLocaleString('vi-VN')} ELO</span>
            <span className="pf-bar pf-bar--rank"><i style={{ width: `${progress}%` }} /></span>
            <small>{next ? `Còn ${next.min - me.elo} ELO lên ${next.name}` : 'Bậc cao nhất'}</small>
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
