/** Thẻ chế độ = ảnh lá bài (sinh bởi scripts/gen_modes.py). */
export default function ModeCard({ mode }) {
  return <img className="mode-card-img" src={mode.image} alt={mode.title} draggable={false} />;
}
