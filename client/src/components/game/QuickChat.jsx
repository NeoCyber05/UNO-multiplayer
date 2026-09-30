import { QUICK_CHAT } from '../../data/mockData';

export default function QuickChat({ onSend, onClose }) {
  return (
    <div className="qchat" role="menu">
      {QUICK_CHAT.map((t) => (
        <button
          key={t}
          type="button"
          role="menuitem"
          className={`qchat__item ${t.length <= 2 ? 'is-emoji' : ''}`}
          onClick={() => {
            onSend(t);
            onClose();
          }}
        >
          {t}
        </button>
      ))}
    </div>
  );
}
