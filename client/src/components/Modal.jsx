import { useEffect } from 'react';

export default function Modal({ open, onClose, children, className = '', label }) {
  useEffect(() => {
    if (!open || !onClose) return undefined;
    const onKey = (e) => e.key === 'Escape' && onClose();
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  if (!open) return null;
  return (
    <div className="modal" onMouseDown={(e) => e.target === e.currentTarget && onClose?.()}>
      <div className={`modal__box ${className}`} role="dialog" aria-modal="true" aria-label={label}>
        {children}
      </div>
    </div>
  );
}
