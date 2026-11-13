import { useEffect, useRef } from 'react';
import { IconArrowRight, IconBack } from '../Icons.jsx';
import ModeCard from './ModeCard.jsx';

const WHEEL_GAP_MS = 250;

/**
 * Carousel xoay vòng các thẻ chế độ.
 * - Bấm thẻ chưa chọn để chọn; bấm thẻ đang chọn để chơi.
 * - Phím ← → đổi thẻ, Enter chọn (tắt khi keyboard=false, vd đang mở modal).
 * - Cuộn chuột đổi 1 thẻ mỗi lần.
 * Hiện bốn thẻ trên một hàng; cuộn cửa sổ khi chọn thẻ ngoài hàng.
 */
export default function ModeCarousel({ modes, active, onChange, onSelect, keyboard = true }) {
  const n = modes.length;
  const visible = Math.min(4, n);
  const start = Math.max(0, Math.min(active - 1, n - visible));
  const lastWheel = useRef(0);
  const track = useRef(null);

  const go = (step) => onChange((active + step + n) % n);

  useEffect(() => {
    if (!keyboard) return undefined;
    const onKey = (e) => {
      if (e.defaultPrevented || e.altKey || e.ctrlKey || e.metaKey || e.target.closest?.('input, textarea, select, [contenteditable="true"]')) return;
      if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        e.preventDefault();
        const next = (active + (e.key === 'ArrowLeft' ? -1 : 1) + n) % n;
        onChange(next);
      } else if (e.key === 'Enter' && !e.target.closest?.('button, a') && !e.repeat) {
        e.preventDefault();
        onSelect(modes[active]);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [keyboard, active, n, modes, onChange, onSelect]);

  useEffect(() => {
    if (track.current?.contains(document.activeElement)) {
      track.current.querySelector('.is-active')?.focus({ preventScroll: true });
    }
  }, [active]);

  const onWheel = (e) => {
    if (e.ctrlKey) return;
    const delta = Math.abs(e.deltaY) >= Math.abs(e.deltaX) ? e.deltaY : e.deltaX;
    const now = Date.now();
    if (Math.abs(delta) < 4 || now - lastWheel.current < WHEEL_GAP_MS) return;
    lastWheel.current = now;
    go(delta > 0 ? 1 : -1);
  };

  return (
    <div className="carousel" onWheel={onWheel}>
      <button type="button" className="carousel__arrow carousel__arrow--left" onClick={() => go(-1)} aria-label="Chế độ trước">
        <IconBack size={28} />
      </button>

      <div className="carousel__track" ref={track}>
        {modes.map((m, i) => {
          const off = i - start - (visible - 1) / 2;
          const abs = Math.abs(off);
          const hidden = i < start || i >= start + visible;
          const selected = i === active;
          return (
            <button
              key={m.key}
              type="button"
              className={`carousel__item ${selected ? 'is-active' : ''} ${hidden ? 'is-hidden' : ''}`}
              style={{ '--off': off, '--abs': abs, zIndex: selected ? 10 : 1 }}
              tabIndex={selected ? 0 : -1}
              aria-hidden={hidden}
              aria-pressed={selected}
              aria-label={selected ? `Chơi ${m.title}` : m.title}
              onClick={() => (selected ? onSelect(m) : onChange(i))}
            >
              <ModeCard mode={m} />
            </button>
          );
        })}
      </div>

      <button type="button" className="carousel__arrow carousel__arrow--right" onClick={() => go(1)} aria-label="Chế độ tiếp">
        <IconArrowRight size={28} />
      </button>
    </div>
  );
}
