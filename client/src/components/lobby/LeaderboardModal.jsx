import { useState } from 'react';
import Modal from '../Modal.jsx';
import Avatar from '../Avatar.jsx';
import RankBadge from '../RankBadge.jsx';
import { IconClose, IconTrophy } from '../Icons.jsx';
import { LEADERBOARD } from '../../data/mockData';
import { RANKS, rankOf } from '../../data/ranks';
import { useSocial } from '../../context/SocialContext.jsx';

export default function LeaderboardModal({ open, onClose }) {
  const { me } = useSocial();
  const [selected, setSelected] = useState(null);
  const tier = selected ?? rankOf(me.elo).key;
  const rank = RANKS.find(r => r.key === tier);
  const next = RANKS[RANKS.indexOf(rank) + 1];
  const players = [...LEADERBOARD.filter(p => p.id !== me.id), me]
    .filter(p => rankOf(p.elo).key === tier).sort((a, b) => b.elo - a.elo);
  return (
    <Modal open={open} onClose={onClose} label="Bảng xếp hạng" className="lobby-modal leaderboard">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconTrophy size={24} /> Bảng xếp hạng</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>
      <nav className="rank-filters" aria-label="Bậc xếp hạng">
        {RANKS.map(r => <button type="button" key={r.key} aria-pressed={tier === r.key} onClick={() => setSelected(r.key)}><RankBadge rank={r} size={62} label /></button>)}
      </nav>
      <div className="lb-summary"><RankBadge rank={rank} size={56} /><div><b className="display">Hạng {rank.name}</b><span>{rank.min.toLocaleString('vi-VN')}{next ? ` – ${(next.min - 1).toLocaleString('vi-VN')}` : '+'} ELO</span></div><small>{players.length} người chơi</small></div>
      <div className="lb-columns"><span>VỊ TRÍ</span><span>NGƯỜI CHƠI</span><span>ELO</span></div>
      <ol className="lb-list" aria-label={`Xếp hạng ${rank.name}`}>
        {players.map((p, i) => <li key={p.id} className={`lb-row ${p.id === me.id ? 'is-me' : ''}`}>
          <span className="lb-row__rank display">{String(i + 1).padStart(2, '0')}</span>
          <Avatar person={p} size={40} />
          <span className="lb-row__name">{p.name}{p.id === me.id && <small>Bạn</small>}</span>
          <RankBadge rank={rank} size={30} />
          <span className="lb-row__elo display">{p.elo.toLocaleString('vi-VN')}</span>
        </li>)}
      </ol>
      {!players.length && <p className="lb-empty">Chưa có người chơi ở bậc này.</p>}
      <div className="lb-foot"><RankBadge elo={me.elo} size={30} /><span>Hạng của bạn: <b>{rankOf(me.elo).name}</b></span><b>{me.elo.toLocaleString('vi-VN')} ELO</b></div>
    </Modal>
  );
}
