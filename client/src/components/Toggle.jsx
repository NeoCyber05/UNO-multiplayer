/** Công tắc bật/tắt. CSS `.toggle` nằm trong styles/theme.css. */
export default function Toggle({ label, hint, value, onChange }) {
  return (
    <label className="toggle">
      <span className="toggle__text">
        <span>{label}</span>
        {hint && <small className="toggle__hint">{hint}</small>}
      </span>
      <input type="checkbox" checked={value} onChange={(e) => onChange(e.target.checked)} />
      <span className="toggle__track"><span className="toggle__thumb" /></span>
    </label>
  );
}
