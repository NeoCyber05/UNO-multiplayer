import Avatar from '../Avatar.jsx';
import { TURN_SECONDS } from '../../game/constants';

export function SpeechBubble({ text, side = 'right' }) {
  return <div className={`bubble bubble--${side}`}>{text}</div>;
}

/**
 * Avatar vuông + bảng tên + số lá + vòng đếm giờ lượt.
 * tone: 'blue' | 'orange' | 'gold' — màu bảng tên
 * arrowSide: 'top' | 'left' — phía đặt mũi tên báo lượt (mũi tên chỉ vào avatar)
 */
export default function PlayerBadge({
  player,
  isTurn,
  turnKey,
  tone = 'blue',
  relation,
  bubble,
  bubbleSide = 'right',
  size = 96,
  arrowSide = 'top',
}) {
  const count = player.hand.length;
  return (
    <div className={`pbadge ${isTurn ? 'is-turn' : ''}`}>
      <div className="pbadge__avatar" style={{ width: size, height: size }}>
        {isTurn && <span key={turnKey} className="pbadge__timer" style={{ '--turn-dur': `${TURN_SECONDS}s` }} />}
        {isTurn && (
          <span className={`turn-arrow turn-arrow--${arrowSide}`} aria-hidden="true">
            <span className="turn-arrow__tag">LƯỢT</span>
            <span className="turn-arrow__tri" />
          </span>
        )}
        <Avatar person={player} size={size} shape="square" />
        <span className="pbadge__count" title={`${count} lá`}>{count}</span>
        {count === 1 && <span className="pbadge__uno">UNO!</span>}
        {bubble && <SpeechBubble key={bubble.id} text={bubble.text} side={bubbleSide} />}
      </div>
      <div className={`pbadge__plate pbadge__plate--${tone}`}>{player.isHuman ? `${player.name} (Bạn)` : player.name}</div>
      {relation && <div className={`pbadge__rel pbadge__rel--${relation.kind}`}>{relation.label}</div>}
    </div>
  );
}
