import { useState } from 'react';
import { ASSETS } from '../../config/assets';

/** Nút CALL UNO 3D. Sáng + nhấp nháy khi được phép hô. */
export default function UnoButton({ active, onClick }) {
  const [burst, setBurst] = useState(0);
  const press = () => {
    setBurst((n) => n + 1);
    onClick();
  };
  return (
    <>
      {burst > 0 && <UnoBurst key={burst} />}
    <button
      type="button"
      className={`uno-btn ${active ? 'is-active' : ''} ${ASSETS.unoButton ? 'uno-btn--img' : ''}`}
      onClick={press}
      disabled={!active}
      aria-label="Hô UNO"
    >
      {ASSETS.unoButton ? (
        <img src={ASSETS.unoButton} alt="" draggable={false} />
      ) : (
        <span className="uno-btn__cap">
          <span className="uno-btn__call">CALL</span>
          <span className="uno-btn__uno">UNO</span>
        </span>
      )}
    </button>
    </>
  );
}

/** Vòng sóng + tia lấp lánh + chữ "UNO!" bung ra từ nút. */
function UnoBurst() {
  return (
    <div className="uno-burst" aria-hidden="true">
      <span className="uno-burst__ring" />
      <span className="uno-burst__ring uno-burst__ring--2" />
      {Array.from({ length: 12 }, (_, i) => (
        <i key={i} className="uno-burst__spark" style={{ '--a': `${i * 30}deg`, '--c': ['#ffd21f', '#ff4d4d', '#4da3ff', '#43d17a'][i % 4] }} />
      ))}
      <span className="uno-burst__text display">UNO!</span>
    </div>
  );
}
