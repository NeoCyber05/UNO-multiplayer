import { useRef } from 'react';
import UnoCard from '../card/UnoCard.jsx';
import { COLOR_HEX, COLOR_LABEL } from '../../game/constants';

// 3 cung mũi tên quanh bàn (theo chiều kim đồng hồ), góc tính bằng độ
const R = 86;
const ARCS = [
  [-80, 10],
  [40, 130],
  [160, 250],
];
const pt = (deg, r = R) => {
  const a = (deg * Math.PI) / 180;
  return [100 + r * Math.cos(a), 100 + r * Math.sin(a)];
};
const ARROWS = ARCS.map(([from, to]) => {
  const [x1, y1] = pt(from);
  const [x2, y2] = pt(to);
  // Đầu mũi tên: đỉnh nhô về phía trước theo tiếp tuyến, đáy rộng theo bán kính
  const tip = pt(to + 9);
  const outer = pt(to, R + 11);
  const inner = pt(to, R - 11);
  return {
    arc: `M${x1} ${y1} A${R} ${R} 0 0 1 ${x2} ${y2}`,
    head: `M${outer.join(' ')} L${tip.join(' ')} L${inner.join(' ')} Z`,
  };
});

function DirectionRing({ direction }) {
  return (
    <div className={`dir-ring ${direction === 1 ? '' : 'is-ccw'}`}>
      <svg viewBox="0 0 200 200" className="dir-ring__svg">
        {ARROWS.map((a) => (
          <g key={a.arc}>
            <path d={a.arc} />
            <path d={a.head} className="dir-ring__head" />
          </g>
        ))}
      </svg>
    </div>
  );
}

// Góc (độ, 0 = sang phải, theo chiều kim đồng hồ) từ tâm bàn tới từng ghế
const SEAT_ANGLE = { right: 0, bottom: 90, left: 180, top: 270 };

/** Mũi tên quanh chồng bài, chỉ về người đang có lượt — luôn quay theo đường ngắn nhất. */
function TurnPointer({ seat }) {
  const angle = useRef(SEAT_ANGLE[seat] ?? 90);
  const target = SEAT_ANGLE[seat];
  if (target != null) {
    const diff = ((((target - angle.current) % 360) + 540) % 360) - 180;
    angle.current += diff;
  }
  if (target == null) return null;
  return (
    <div className="turn-pointer" style={{ '--a': `${angle.current}deg` }}>
      <span className="turn-pointer__tri" />
    </div>
  );
}

/** Khu vực giữa bàn: chồng rút, chồng đánh, vòng chiều đi, màu hiện tại. */
export default function CenterTable({ discard, currentColor, direction, deckCount, canDraw, onDraw, turnSeat }) {
  const visible = discard.slice(-4);
  const colorHex = COLOR_HEX[currentColor];

  return (
    <div className="center" style={{ '--cur': colorHex }}>
      <div className="center__glow" />
      <DirectionRing direction={direction} />
      <TurnPointer seat={turnSeat} />

      <div className="center__color">
        <span className="dot" style={{ background: colorHex, width: 12, height: 12 }} />
        Màu: {COLOR_LABEL[currentColor]}
      </div>

      <div className="center__piles">
        <button
          type="button"
          className={`deck ${canDraw ? 'can-draw' : ''}`}
          onClick={onDraw}
          disabled={!canDraw}
          aria-label="Rút bài"
        >
          <UnoCard faceDown width={104} className="deck__c deck__c--3" />
          <UnoCard faceDown width={104} className="deck__c deck__c--2" />
          <UnoCard faceDown width={104} className="deck__c deck__c--1" />
          <span className="deck__label">{canDraw ? 'Rút bài' : `${deckCount} lá`}</span>
        </button>

        <div className="discard">
          {visible.map((card, i) => {
            const isTop = i === visible.length - 1;
            return (
              <div
                key={card.id}
                className={`discard__card ${isTop ? 'is-top' : ''}`}
                style={{ '--tilt': `${card.tilt}deg` }}
              >
                <UnoCard card={card} width={112} />
              </div>
            );
          })}
        </div>
      </div>
    </div>
  );
}
