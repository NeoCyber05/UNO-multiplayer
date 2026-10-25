import UnoCard from '../card/UnoCard.jsx';
import useDealDelays from '../../hooks/useDealDelays';

// Bot bị +2/+4 -> các lá bay vào lần lượt cách nhau chừng này giây
const DEAL_STAGGER = 0.28;

/**
 * Xòe bài của người chơi khác (úp hoặc ngửa).
 * Bọc trong .fan-wrap với --rot để xoay cho ghế trái/phải.
 */
export default function Fan({ cards, faceUp = false, cardWidth = 60, step = 24, maxLength = 300 }) {
  const n = cards.length;
  const gap = n > 1 ? Math.min(step, (maxLength - cardWidth) / (n - 1)) : 0;
  const angle = Math.min(5, 44 / Math.max(n, 1));
  const delays = useDealDelays(cards, DEAL_STAGGER);

  return (
    <div className="fan" style={{ width: cardWidth + gap * (n - 1), height: cardWidth * 1.5 }}>
      {cards.map((card, i) => {
        const off = i - (n - 1) / 2;
        return (
          <div
            key={card.id}
            className="fan__card"
            style={{ left: i * gap, transform: `translateY(${off * off * 0.8}px) rotate(${off * angle}deg)` }}
          >
            <UnoCard card={card} faceDown={!faceUp} width={cardWidth} style={{ animationDelay: `${delays.get(card.id)}s` }} />
          </div>
        );
      })}
    </div>
  );
}
