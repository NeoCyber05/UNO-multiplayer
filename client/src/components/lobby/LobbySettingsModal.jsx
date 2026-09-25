import { useState } from 'react';
import Modal from '../Modal.jsx';
import Toggle from '../Toggle.jsx';
import { IconCards, IconClose, IconGear, IconLogout, IconMusic, IconShield, IconSound } from '../Icons.jsx';

const SECTIONS = [
  { key: 'sound', label: 'Âm thanh', icon: IconSound },
  { key: 'game', label: 'Trò chơi', icon: IconCards },
  { key: 'account', label: 'Tài khoản', icon: IconShield },
];

const ANIM_SPEEDS = [
  { value: 'slow', label: 'Chậm' },
  { value: 'normal', label: 'Vừa' },
  { value: 'fast', label: 'Nhanh' },
];

const LANGUAGES = [
  { value: 'vi', label: 'Tiếng Việt' },
  { value: 'en', label: 'English' },
];

function Slider({ icon: Icon, label, value, onChange, disabled }) {
  return (
    <label className={`slider ${disabled ? 'is-disabled' : ''}`}>
      <span className="slider__head">
        <span className="slider__label"><Icon size={18} /> {label}</span>
        <span className="slider__value display">{value}%</span>
      </span>
      <input
        type="range"
        min={0}
        max={100}
        value={value}
        disabled={disabled}
        style={{ '--val': `${value}%` }}
        onChange={(e) => onChange(Number(e.target.value))}
      />
    </label>
  );
}

function Segmented({ label, options, value, onChange }) {
  return (
    <div className="set-row">
      <span>{label}</span>
      <div className="seg seg--sm">
        {options.map((o) => (
          <button
            key={o.value}
            type="button"
            className={`seg__btn ${value === o.value ? 'is-on' : ''}`}
            onClick={() => onChange(o.value)}
          >
            {o.label}
          </button>
        ))}
      </div>
    </div>
  );
}

export default function LobbySettingsModal({ open, onClose, settings, onChange, onReset, onLogout }) {
  const [section, setSection] = useState('sound');
  const s = settings;

  return (
    <Modal open={open} onClose={onClose} label="Cài đặt" className="lobby-modal lobby-settings">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconGear size={24} /> Cài đặt</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>

      <div className="lobby-settings__body">
        <nav className="set-nav">
          {SECTIONS.map(({ key, label, icon: Icon }) => (
            <button
              key={key}
              type="button"
              className={`set-nav__item ${section === key ? 'is-on' : ''}`}
              onClick={() => setSection(key)}
            >
              <Icon size={19} /> {label}
            </button>
          ))}
          <button type="button" className="set-nav__reset" onClick={onReset}>Khôi phục mặc định</button>
        </nav>

        <div key={section} className="set-panel">
          {section === 'sound' && (
            <>
              <div className="set-group">
                <Toggle label="Tắt toàn bộ âm thanh" value={s.muted} onChange={(v) => onChange('muted', v)} />
              </div>
              <div className="set-group">
                <Slider icon={IconSound} label="Hiệu ứng" value={s.sfxVolume} disabled={s.muted} onChange={(v) => onChange('sfxVolume', v)} />
                <Slider icon={IconMusic} label="Nhạc nền" value={s.musicVolume} disabled={s.muted} onChange={(v) => onChange('musicVolume', v)} />
              </div>
              <div className="set-group">
                <Toggle label="Rung khi tới lượt" hint="Chỉ trên thiết bị hỗ trợ" value={s.vibrate} onChange={(v) => onChange('vibrate', v)} />
              </div>
            </>
          )}

          {section === 'game' && (
            <>
              <div className="set-group">
                <Toggle label="Tự xếp bài trên tay" hint="Gom theo màu, rồi theo số" value={s.autoSort} onChange={(v) => onChange('autoSort', v)} />
                <Toggle label="Làm nổi lá đánh được" value={s.highlightPlayable} onChange={(v) => onChange('highlightPlayable', v)} />
                <Toggle label="Xác nhận trước khi đánh Wild +4" value={s.confirmWild} onChange={(v) => onChange('confirmWild', v)} />
              </div>
              <div className="set-group">
                <Segmented label="Tốc độ hiệu ứng" options={ANIM_SPEEDS} value={s.animSpeed} onChange={(v) => onChange('animSpeed', v)} />
              </div>
            </>
          )}

          {section === 'account' && (
            <>
              <div className="set-group">
                <Segmented label="Ngôn ngữ" options={LANGUAGES} value={s.language} onChange={(v) => onChange('language', v)} />
              </div>
              <div className="set-group">
                <Toggle label="Nhận lời mời vào phòng" value={s.friendInvites} onChange={(v) => onChange('friendInvites', v)} />
                <Toggle label="Hiện trạng thái online" hint="Bạn bè thấy bạn đang ở sảnh / trong trận" value={s.showOnline} onChange={(v) => onChange('showOnline', v)} />
              </div>
              <button type="button" className="btn btn--danger btn--block" onClick={onLogout}>
                <IconLogout /> Đăng xuất
              </button>
            </>
          )}
        </div>
      </div>
    </Modal>
  );
}
