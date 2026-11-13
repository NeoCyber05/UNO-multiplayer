import { useState } from 'react';
import { ASSETS } from '../../config/assets';
import UnoCard from '../card/UnoCard.jsx';
import { COLORS } from '../../game/constants';

export default function UnoButton({ active, calls, onClick }) {
  return (
    <>
      {calls > 0 && <UnoBurst key={calls} />}
      <button
        type="button"
        className={`uno-btn ${active ? 'is-active' : ''} ${ASSETS.unoButton ? 'uno-btn--img' : ''}`}
        onClick={onClick}
        disabled={!active}
        aria-label="Hô UNO"
      >
        {ASSETS.unoButton ? (
          <img src={ASSETS.unoButton} alt="" draggable={false} />
        ) : (
          <span className="uno-btn__cap">
            <span className="uno-btn__call">HÔ</span>
            <span className="uno-btn__uno">UNO</span>
          </span>
        )}
      </button>
    </>
  );
}

function UnoBurst() {
  const [finished, setFinished] = useState(false);
  if (finished) return null;

  return (
    <div
      className="uno-burst"
      aria-hidden="true"
      onAnimationEnd={(event) => {
        if (event.animationName === 'uno-dismiss') setFinished(true);
      }}
    >
      <span className="uno-burst__ring" />
      <div className="uno-burst__stamp">
        {COLORS.map((color, i) => (
          <div key={color} className="uno-burst__card" style={{ '--offset': i - 1.5 }}>
            <UnoCard card={{ color, value: '1' }} width={88} />
          </div>
        ))}
        <strong className="uno-burst__text display">UNO!</strong>
      </div>
    </div>
  );
}
