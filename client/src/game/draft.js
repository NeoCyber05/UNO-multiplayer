export function createDraft(cards) {
  if (cards.length < 32) throw new Error('Draft cần ít nhất 32 lá bài.');
  return {
    hands: Array.from({ length: 4 }, (_, seat) => cards.slice(seat * 4, seat * 4 + 4)),
    offers: cards.slice(16, 20),
    deck: cards.slice(20),
    turn: 0,
  };
}

export function draftSeat(state) {
  return state.turn >= 16 ? null : (Math.floor(state.turn / 4) + state.turn % 4) % 4;
}

export function pickDraftCard(state, index) {
  const seat = draftSeat(state);
  if (seat === null || !Number.isInteger(index) || index < 0 || index > 3 || !state.offers[index]) return state;
  const turn = state.turn + 1;
  const nextRound = turn < 16 && turn % 4 === 0;
  return {
    hands: state.hands.map((hand, i) => i === seat ? [...hand, state.offers[index]] : hand),
    offers: nextRound ? state.deck.slice(0, 4) : state.offers.map((card, i) => i === index ? null : card),
    deck: nextRound ? state.deck.slice(4) : state.deck,
    turn,
  };
}
