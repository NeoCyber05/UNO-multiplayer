import { useEffect, useRef } from 'react';

export default function Modal({ open, onClose, children, className = '', label }) {
  const boxRef = useRef(null);
  const closeRef = useRef(onClose);
  closeRef.current = onClose;

  useEffect(() => {
    if (!open) return undefined;
    const previous = document.activeElement;
    const box = boxRef.current;
    const focusable = () => [...box.querySelectorAll('button, input, select, textarea, a[href], [tabindex]')]
      .filter((el) => !el.disabled && el.tabIndex >= 0 && el.getClientRects().length);
    const focusFirst = () => (focusable()[0] ?? box).focus();
    focusFirst();
    const onKey = (e) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        closeRef.current?.();
      }
      if (e.key !== 'Tab') return;
      const items = focusable();
      const next = e.shiftKey ? items.at(-1) : items[0];
      const edge = e.shiftKey ? items[0] : items.at(-1);
      if (!items.length || document.activeElement === edge || document.activeElement === box) {
        e.preventDefault();
        (next ?? box).focus();
      }
    };
    const onFocus = (e) => { if (!box.contains(e.target)) focusFirst(); };
    document.addEventListener('keydown', onKey);
    document.addEventListener('focusin', onFocus);
    return () => {
      document.removeEventListener('keydown', onKey);
      document.removeEventListener('focusin', onFocus);
      if (previous?.isConnected) previous.focus();
    };
  }, [open]);

  return (
    <div className="modal" data-open={open} aria-hidden={!open} inert={open ? undefined : ''} onMouseDown={(e) => e.target === e.currentTarget && onClose?.()}>
      <div ref={boxRef} tabIndex={-1} className={`modal__box ${className}`} role="dialog" aria-modal="true" aria-label={label}>
        {children}
      </div>
    </div>
  );
}
