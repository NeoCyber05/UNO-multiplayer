import { useState } from 'react';
import Modal from '../Modal.jsx';
import Avatar from '../Avatar.jsx';
import { IconClose, IconPlus, IconUsers } from '../Icons.jsx';
import { useSocial } from '../../context/SocialContext.jsx';

const initialsOf = (name) => name.trim().slice(0, 2).toUpperCase() || '??';
const TINTS = ['#4C86FF', '#35D399', '#FF5262', '#22C3D6', '#FFC94A'];

export default function FriendsModal({ open, onClose, notify, onInvite }) {
  const { friends: list, sendFriendRequest } = useSocial();
  const [addValue, setAddValue] = useState('');

  const online = list.filter((f) => f.online && !f.pending).length;

  const addFriend = (e) => {
    e.preventDefault();
    const name = addValue.trim();
    if (!name) return;
    if (list.some((f) => f.name.toLowerCase() === name.toLowerCase())) {
      notify(`${name} đã là bạn bè`);
      setAddValue('');
      return;
    }
    sendFriendRequest({
      id: `f-${Date.now()}`,
      name,
      initials: initialsOf(name),
      tint: TINTS[list.length % TINTS.length],
    });
    notify(`Đã gửi lời mời kết bạn tới ${name}`);
    setAddValue('');
  };

  const invite = (f) => {
    onInvite(f);
    onClose();
  };

  return (
    <Modal open={open} onClose={onClose} label="Bạn bè" className="lobby-modal">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconUsers size={24} /> Bạn bè</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>

      <form className="friends-add" onSubmit={addFriend}>
        <input
          className="input"
          placeholder="Nhập tên hoặc tag để kết bạn"
          value={addValue}
          onChange={(e) => setAddValue(e.target.value)}
          aria-label="Tên hoặc tag bạn bè"
        />
        <button type="submit" className="btn btn--ghost" aria-label="Kết bạn">
          <IconPlus size={16} /> Kết bạn
        </button>
      </form>

      <div className="section-title">BẠN BÈ · {online} ONLINE</div>
      <div className="friend-list">
        {list.map((f) => (
          <div key={f.id} className={`friend ${f.online ? '' : 'is-offline'}`}>
            <div className="friend__avatar">
              <Avatar person={f} size={40} />
              <span className={`friend__status ${f.online ? 'is-on' : ''}`} />
            </div>
            <div style={{ flexGrow: 1, minWidth: 0 }}>
              <div style={{ fontWeight: 700, fontSize: 14 }}>{f.name}</div>
              <div className="muted friend__sub">{f.status}</div>
            </div>
            {f.online && !f.pending && (
              <button type="button" className="btn btn--sm btn--ghost" onClick={() => invite(f)}>Mời</button>
            )}
          </div>
        ))}
      </div>
    </Modal>
  );
}
