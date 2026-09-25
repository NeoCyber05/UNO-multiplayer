import { useEffect, useRef } from 'react';
import { IconArrowRight, IconBack } from '../Icons.jsx';
import ModeCard from './ModeCard.jsx';

const WHEEL_GAP_MS = 250;

// Khoảng cách vòng ngắn nhất từ `active` tới `i` (vd n=4, active=0, i=3 -> -1).
function offsetOf(i, active, n) {
  let d = (i - active) % n;
  if (d < 0) d += n;
  if (d > n / 2) d -= n;
  return d;
}

/**
 * Carousel xoay vòng các thẻ chế độ.
 * - Bấm thẻ bên -> xoay tới; bấm thẻ giữa -> onSelect.
 * - Phím ← → đổi thẻ, Enter chọn (tắt khi keyboard=false, vd đang mở modal).
 * - Cuộn chuột đổi 1 thẻ mỗi lần.
 * Hiện thẻ giữa ± 1; từ 5 thẻ trở lên hiện thêm ± 2.
 */
export default function ModeCarousel({ modes, active, onChange, onSelect, keyboard = true }) {
  const n = modes.length;
  const reach = n >= 5 ? 2 : 1;
  const lastWheel = useRef(0);

  const go = (step) => onChange((active + step + n) % n);

  useEffect(() => {
    if (!keyboard) return undefined;
    const onKey = (e) => {
      if (e.target.closest?.('input, textarea, select')) return;
      if (e.key === 'ArrowLeft') onChange((active - 1 + n) % n);
      else if (e.key === 'ArrowRight') onChange((active + 1) % n);
      else if (e.key === 'Enter' && !e.repeat) {
        // Nút khác (dock, modal...) đang focus thì để nút đó tự xử lý Enter
        if (e.target.closest?.('button:not(.carousel__item)')) return;
        e.preventDefault(); // chặn click mặc định của thẻ đang focus -> không vào 2 lần
        onSelect(modes[active]);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [keyboard, active, n, modes, onChange, onSelect]);

  const onWheel = (e) => {
    const delta = Math.abs(e.deltaY) >= Math.abs(e.deltaX) ? e.deltaY : e.deltaX;
    const now = Date.now();
    if (Math.abs(delta) < 4 || now - lastWheel.current < WHEEL_GAP_MS) return;
    lastWheel.current = now;
    go(delta > 0 ? 1 : -1);
  };

  return (
    // preventDefault ở mousedown: bấm chuột không giữ focus trên thẻ/mũi tên,
    // nên phím ← → Enter luôn áp dụng cho thẻ đang ở giữa.
    <div className="carousel" onWheel={onWheel} onMouseDown={(e) => e.preventDefault()}>
      <button type="button" className="carousel__arrow carousel__arrow--left" tabIndex={-1} onClick={() => go(-1)} aria-label="Chế độ trước">
        <IconBack size={28} />
      </button>

      <div className="carousel__track">
        {modes.map((m, i) => {
          const off = offsetOf(i, active, n);
          const abs = Math.abs(off);
          const hidden = abs > reach;
          return (
            <button
              key={m.key}
              type="button"
              className={`carousel__item ${off === 0 ? 'is-active' : ''} ${hidden ? 'is-hidden' : ''}`}
              style={{ '--off': off, '--abs': abs, zIndex: 10 - abs }}
              tabIndex={hidden ? -1 : 0}
              aria-hidden={hidden}
              aria-label={off === 0 ? `Chơi ${m.title}` : m.title}
              onClick={() => (off === 0 ? onSelect(m) : onChange(i))}
            >
              <ModeCard mode={m} />
            </button>
          );
        })}
      </div>

      <button type="button" className="carousel__arrow carousel__arrow--right" tabIndex={-1} onClick={() => go(1)} aria-label="Chế độ tiếp">
        <IconArrowRight size={28} />
      </button>
    </div>
  );
}
