import { ASSETS } from '../../config/assets';

/** Nút CALL UNO 3D. Sáng + nhấp nháy khi được phép hô. */
export default function UnoButton({ active, onClick }) {
  return (
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
          <span className="uno-btn__call">CALL</span>
          <span className="uno-btn__uno">UNO</span>
        </span>
      )}
    </button>
  );
}
