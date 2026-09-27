import { useState } from 'react';
import Modal from '../Modal.jsx';
import Toggle from '../Toggle.jsx';
import { IconClose } from '../Icons.jsx';

export default function SettingsModal({ open, onClose, onRestart, onLeave }) {
  const [sound, setSound] = useState(true);
  const [music, setMusic] = useState(false);
  const [vibrate, setVibrate] = useState(true);

  return (
    <Modal open={open} onClose={onClose} className="settings" label="Cài đặt">
      <div className="settings__head">
        <div className="display settings__title">Cài đặt</div>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>
      <div className="settings__group">
        <Toggle label="Hiệu ứng âm thanh" value={sound} onChange={setSound} />
        <Toggle label="Nhạc nền" value={music} onChange={setMusic} />
        <Toggle label="Rung khi tới lượt" value={vibrate} onChange={setVibrate} />
      </div>
      <div className="settings__rules">
        <b>Luật nhanh:</b> đánh lá cùng màu hoặc cùng số/ký hiệu. Không có lá phù hợp thì rút 1 lá.
        Còn 2 lá hãy bấm <b>CALL UNO</b> trước khi đánh (hoặc trong 3 giây sau khi đánh) để tránh bị phạt +2.
      </div>
      <div className="settings__actions">
        <button type="button" className="btn btn--danger" onClick={onLeave}>Rời trận</button>
        <button type="button" className="btn btn--ghost" onClick={onRestart}>Chơi lại</button>
        <button type="button" className="btn btn--primary" onClick={onClose}>Tiếp tục</button>
      </div>
    </Modal>
  );
}
