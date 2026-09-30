import Avatar from '../Avatar.jsx';
import { IconGear, IconMute, IconSound, IconTrophy } from '../Icons.jsx';
import { ASSETS } from '../../config/assets';

/** Thanh trên Sảnh: thẻ hồ sơ (bấm mở hồ sơ), logo, nút âm thanh + cài đặt. */
export default function TopBar({ me, muted, onToggleMute, onProfile, onSettings }) {
  return (
    <header className="topbar">
      <button type="button" className="topbar__profile" onClick={onProfile} aria-label="Hồ sơ">
        <Avatar person={me} size={58} shape="square" />
        <span className="topbar__info">
          <span className="topbar__row">
            <span className="topbar__name">{me.name}</span>
            <span className="topbar__elo">
              <IconTrophy size={15} /> {me.elo.toLocaleString('vi-VN')}
            </span>
          </span>
          <span className="topbar__xp"><i style={{ width: `${me.xp}%` }} /></span>
          <span className="topbar__lv">Cấp {me.level} · {me.xp}% XP</span>
        </span>
      </button>

      <div className="topbar__logo">
        {ASSETS.logo ? <img src={ASSETS.logo} alt="UNO" /> : <span className="display">UNO</span>}
      </div>

      <div className="topbar__actions">
        <button type="button" className="topbar__btn" onClick={onToggleMute} aria-label={muted ? 'Bật âm thanh' : 'Tắt âm thanh'}>
          {muted ? <IconMute size={22} /> : <IconSound size={22} />}
        </button>
        <button type="button" className="topbar__btn" onClick={onSettings} aria-label="Cài đặt">
          <IconGear size={22} />
        </button>
      </div>
    </header>
  );
}
