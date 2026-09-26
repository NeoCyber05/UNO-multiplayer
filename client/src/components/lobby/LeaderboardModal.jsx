import Modal from '../Modal.jsx';
import Avatar from '../Avatar.jsx';
import { IconClose, IconTrophy } from '../Icons.jsx';
import { LEADERBOARD, ME } from '../../data/mockData';

export default function LeaderboardModal({ open, onClose }) {
  return (
    <Modal open={open} onClose={onClose} label="Bảng xếp hạng" className="lobby-modal">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconTrophy size={24} /> Bảng xếp hạng ELO</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>
      <ol className="lb-list">
        {LEADERBOARD.map((p, i) => (
          <li key={p.id} className={`lb-row ${p.id === ME.id ? 'is-me' : ''} ${i < 3 ? `is-top-${i + 1}` : ''}`}>
            <span className="lb-row__rank display">{i + 1}</span>
            <Avatar person={p} size={36} />
            <span className="lb-row__name">{p.id === ME.id ? `${p.name} (Bạn)` : p.name}</span>
            <span className="lb-row__elo display">{p.elo.toLocaleString('vi-VN')}</span>
          </li>
        ))}
      </ol>
    </Modal>
  );
}
