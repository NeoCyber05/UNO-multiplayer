import { useMemo } from 'react';
import UnoCard from '../card/UnoCard.jsx';
import { sortHand } from '../../game/deck';
import { COLOR_LABEL } from '../../game/constants';
import useDealDelays from '../../hooks/useDealDelays';

const CARD_W = 116;
const MAX_W = 780;
// Nhiều lá vào tay cùng lúc (chia bài, bị +2/+4) -> bay vào lần lượt cách nhau chừng này giây
const DEAL_STAGGER = 0.08;
const VALUE_LABEL = {
  skip: 'Mất lượt', reverse: 'Đảo chiều', draw2: 'Rút hai',
  discard_all: 'Bỏ hết bài cùng màu', wild: 'Đổi màu', wild4: 'Đổi màu rút bốn',
};

/** Bài trên tay người chơi — xòe hình quạt, nhấc lên khi hover, sáng viền lá đánh được. */
export default function MyHand({ cards, playableIds, isMyTurn, drawnCardId, onPlay }) {
  const sorted = useMemo(() => sortHand(cards), [cards]);
  const delays = useDealDelays(cards, DEAL_STAGGER);
  const n = sorted.length;
  const step = n > 1 ? Math.min(80, (MAX_W - CARD_W) / (n - 1)) : 0;
  const angle = Math.min(4.5, 40 / Math.max(n, 1));
  const width = CARD_W + step * (n - 1);

  return (
    <div className={`hand ${isMyTurn ? 'is-turn' : 'is-idle'}`} style={{ width }}>
      {sorted.map((card, i) => {
        const off = i - (n - 1) / 2;
        const playable = playableIds.has(card.id);
        const value = VALUE_LABEL[card.value] ?? card.value;
        const label = card.color === 'wild' ? value : `${value} · ${COLOR_LABEL[card.color]}`;
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
            style={{ left: i * step, '--stack': i, '--r': `${off * angle}deg`, '--y': `${Math.min(off * off * 1.1, 18)}px`, animationDelay: `${delays.get(card.id)}s` }}
            onClick={() => onPlay(card.id)}
            disabled={!isMyTurn || !playable}
            aria-label={label}
          >
            <UnoCard card={card} width={CARD_W} />
          </button>
        );
      })}
    </div>
  );
}
