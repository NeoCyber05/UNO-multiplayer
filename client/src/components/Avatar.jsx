import { ASSETS } from '../config/assets';
import { IconBot } from './Icons.jsx';
import { avatarOf } from '../data/avatars.jsx';

/**
 * Ảnh đại diện. Nếu có ảnh trong ASSETS.avatars[person.id] thì hiện ảnh,
 * bot hiện icon robot, còn lại hiện chữ viết tắt trên nền gradient.
 */
export default function Avatar({ person, size = 40, shape = 'circle', className = '', style }) {
  const src = person && ASSETS.avatars[person.id];
  const preset = person && avatarOf(person.avatar);
  return (
    <div
      className={`avatar avatar--${shape} ${person ? '' : 'avatar--empty'} ${className}`}
      style={{ width: size, height: size, fontSize: size * 0.36, '--tint': preset?.bg ?? person?.tint ?? '#1D3864', ...style }}
    >
      {preset ? (
        <svg viewBox="0 0 64 64" width="100%" height="100%" aria-label={person.name}>{preset.el}</svg>
      ) : src ? (
        <img src={src} alt={person.name} draggable={false} />
      ) : person?.isBot ? (
        <IconBot size={size * 0.58} level={person.botLevel} />
      ) : (
        person?.initials ?? '?'
      )}
    </div>
  );
}
