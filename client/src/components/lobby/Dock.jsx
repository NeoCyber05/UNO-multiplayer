/** Hàng nút dưới đáy Sảnh. items: [{ key, label, icon, onClick }] */
export default function Dock({ items }) {
  return (
    <nav className="dock">
      {items.map(({ key, label, icon: Icon, onClick }) => (
        <button key={key} type="button" className="dock__item" onClick={onClick}>
          <span className="dock__icon"><Icon size={28} /></span>
          <span className="dock__label">{label}</span>
        </button>
      ))}
    </nav>
  );
}
