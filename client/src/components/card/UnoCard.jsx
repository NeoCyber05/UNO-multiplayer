import { ASSETS } from '../../config/assets';
import './UnoCard.css';

/**
 * Lá bài UNO, ảnh lấy từ ASSETS.cardFace / ASSETS.cardBack (sinh bởi scripts/gen_cards.py).
 * @param width chiều rộng lá bài (px), chiều cao = width * 1.5
 */
export default function UnoCard({ card, faceDown = false, width = 100, className = '', style, ...rest }) {
  const vars = { '--cw': `${width}px`, ...style };

  if (faceDown || !card) {
    return (
      <div className={`uno-card uno-card--back ${className}`} style={vars} {...rest}>
        <img className="uno-card__img" src={ASSETS.cardBack} alt="" draggable={false} />
      </div>
    );
  }

  return (
    <div className={`uno-card ${className}`} data-color={card.color} data-value={card.value} style={vars} {...rest}>
      <img className="uno-card__img" src={ASSETS.cardFace(card)} alt={`${card.color} ${card.value}`} draggable={false} />
    </div>
  );
}
