import Modal from '../Modal.jsx';
import { COLORS, COLOR_HEX, COLOR_LABEL } from '../../game/constants';

export default function ColorPicker({ open, onPick, onCancel }) {
  return (
    <Modal open={open} onClose={onCancel} className="cpick-modal" label="Chọn màu">
      <div className="display cpick-modal__title">Chọn màu tiếp theo</div>
      <div className="cpick">
        {COLORS.map((c) => (
          <button
            key={c}
            type="button"
            className="cpick__btn"
            style={{ background: COLOR_HEX[c] }}
            onClick={() => onPick(c)}
            aria-label={COLOR_LABEL[c]}
          >
            <span>{COLOR_LABEL[c]}</span>
          </button>
        ))}
        <span className="cpick__hub">UNO</span>
      </div>
      <button type="button" className="btn btn--outline btn--sm" onClick={onCancel}>
        Huỷ
      </button>
    </Modal>
  );
}
