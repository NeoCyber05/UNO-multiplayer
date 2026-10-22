import { COLORS } from './constants';
import { nextIndex, playableCards } from './rules';

function favoriteColor(hand, excludeId) {
  const count = Object.fromEntries(COLORS.map((c) => [c, 0]));
  hand.forEach((c) => {
    if (c.id !== excludeId && c.color !== 'wild') count[c.color]++;
  });
  return COLORS.reduce((best, c) => (count[c] > count[best] ? c : best), COLORS[Math.floor(Math.random() * 4)]);
}

/** AI đơn giản: ưu tiên phạt đối thủ, tránh phạt đồng đội, giữ lá Wild đến cuối. */
export function chooseBotMove(s, idx) {
  const me = s.players[idx];
  const options = playableCards(s, idx);
  if (!options.length) return s.hasDrawn ? { type: 'PASS', player: idx } : { type: 'DRAW', player: idx };

  const next = s.players[nextIndex(s, idx)];
  const friendly = s.teams && next.team === me.team;
  const nextAlmostOut = next.hand.length <= 2;

  const score = (c) => {
    let v = Math.random();
    if (c.color === s.currentColor) v += 1;
    if (c.value === 'skip' || c.value === 'draw2') v += friendly ? -4 : nextAlmostOut ? 6 : 3;
    if (c.value === 'reverse') v += 1;
    if (c.value === 'discard_all') v += me.hand.filter((h) => h.color === c.color).length * 1.5;
    if (c.value === 'wild') v -= 2;
    if (c.value === 'wild4') v += friendly ? -8 : nextAlmostOut ? 7 : -1;
    return v;
  };

  const card = options.reduce((best, c) => (score(c) > score(best) ? c : best));
  return {
    type: 'PLAY',
    player: idx,
    cardId: card.id,
    color: card.color === 'wild' ? favoriteColor(me.hand, card.id) : undefined,
  };
}
