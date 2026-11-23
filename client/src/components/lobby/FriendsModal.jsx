import { useState } from 'react';
import Modal from '../Modal.jsx';
import Avatar from '../Avatar.jsx';
import { IconClose, IconPlus, IconUsers } from '../Icons.jsx';
import { useSocial } from '../../context/SocialContext.jsx';
import { useSession } from '../../net/session';

export default function FriendsModal({ open, onClose, notify, onInvite }) {
  const { friends: list, sendFriendRequest } = useSocial();
  const { request, connection } = useSession();
  const [addValue, setAddValue] = useState('');

  const online = list.filter((f) => f.online && !f.pending).length;

  const addFriend = async (e) => {
    e.preventDefault();
    const tag = addValue.trim().toUpperCase();
    if (!/^UNO#[0-9]+$/.test(tag)) return notify('Nhập tag dạng UNO#123');
    try {
      await sendFriendRequest({ tag });
      notify(`Đã gửi lời mời kết bạn tới ${tag}`);
      setAddValue('');
    } catch (error) {
      notify(error.message);
    }
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
          placeholder="Nhập tag để kết bạn"
          value={addValue}
          onChange={(e) => setAddValue(e.target.value)}
          aria-label="Tag bạn bè"
        />
        <button type="submit" className="btn btn--ghost" aria-label="Kết bạn" disabled={connection !== 'connected'}>
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
            {f.requestId && f.incoming && [true, false].map((accept) => (
              <button key={String(accept)} type="button" className="btn btn--sm btn--ghost" onClick={() => {
                request('FRIEND_REPLY', { requestId: f.requestId, accept }).catch((error) => notify(error.message));
              }}>{accept ? 'Chấp nhận' : 'Từ chối'}</button>
            ))}
          </div>
        ))}
      </div>
    </Modal>
  );
}
