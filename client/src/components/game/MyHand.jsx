import { useMemo } from 'react';
import UnoCard from '../card/UnoCard.jsx';
import { sortHand } from '../../game/deck';

const CARD_W = 116;
const MAX_W = 780;

/** Bài trên tay người chơi — xòe hình quạt, nhấc lên khi hover, sáng viền lá đánh được. */
export default function MyHand({ cards, playableIds, isMyTurn, drawnCardId, onPlay }) {
  const sorted = useMemo(() => sortHand(cards), [cards]);
  const n = sorted.length;
  const step = n > 1 ? Math.min(80, (MAX_W - CARD_W) / (n - 1)) : 0;
  const angle = Math.min(4.5, 40 / Math.max(n, 1));
  const width = CARD_W + step * (n - 1);

  return (
    <div className={`hand ${isMyTurn ? 'is-turn' : 'is-idle'}`} style={{ width }}>
      {sorted.map((card, i) => {
        const off = i - (n - 1) / 2;
        const playable = playableIds.has(card.id);
        const cls = [
          'hand__card',
          isMyTurn && playable && 'is-playable',
          isMyTurn && !playable && 'is-dim',
          card.id === drawnCardId && 'is-drawn',
        ]
          .filter(Boolean)
          .join(' ');
        return (
          <button
            key={card.id}
            type="button"
            className={cls}
            style={{ left: i * step, zIndex: i, '--r': `${off * angle}deg`, '--y': `${off * off * 1.1}px` }}
            onClick={() => playable && onPlay(card.id)}
            disabled={!isMyTurn}
            aria-label={`${card.color} ${card.value}${playable ? ' — đánh được' : ''}`}
          >
            <UnoCard card={card} width={CARD_W} />
          </button>
        );
      })}
    </div>
  );
}
