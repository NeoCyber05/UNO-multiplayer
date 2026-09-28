export const isWild = (card) => card.color === 'wild';

export function canPlay(card, top, currentColor) {
  if (isWild(card)) return true;
  return card.color === currentColor || card.value === top.value;
}

export const topCard = (state) => state.discard[state.discard.length - 1];

export function nextIndex(state, from, steps = 1) {
  const n = state.players.length;
  return (((from + state.direction * steps) % n) + n) % n;
}

/** Danh sách lá người chơi có thể đánh ở trạng thái hiện tại. */
export function playableCards(state, playerIdx) {
  if (state.turn !== playerIdx || state.winner) return [];
  const top = topCard(state);
  return state.players[playerIdx].hand.filter(
    (c) => canPlay(c, top, state.currentColor) && (!state.hasDrawn || c.id === state.drawnCardId),
  );
}
