import { describe, expect, it } from 'vitest';
import { createDraft, draftSeat, pickDraftCard } from './draft';

const cards = Array.from({ length: 40 }, (_, i) => `card-${i}`);

describe('Draft preview: four rotating rounds', () => {
  it('deals four private cards each and exposes four offers', () => {
    const state = createDraft(cards);
    expect(state.hands.map(hand => hand.length)).toEqual([4, 4, 4, 4]);
    expect(state.offers).toEqual(cards.slice(16, 20));
    expect(state.deck).toHaveLength(20);
    expect(cards).toHaveLength(40);
  });

  it('rotates the first seat each round and ends with eight cards each', () => {
    let state = createDraft(cards);
    const seats = [];
    for (let turn = 0; turn < 16; turn++) {
      seats.push(draftSeat(state));
      const index = state.offers.findIndex(Boolean);
      const picked = state.offers[index];
      const previous = state;
      state = pickDraftCard(state, index);
      expect(state.hands[seats.at(-1)].at(-1)).toBe(picked);
      expect(previous.offers[index]).toBe(picked);
      expect(state.offers).toHaveLength(4);
      const inRound = (turn + 1) % 4;
      expect(state.offers.filter(Boolean)).toHaveLength(turn === 15 ? 0 : inRound === 0 ? 4 : 4 - inRound);
      if (inRound !== 0 || turn === 15) expect(state.offers[index]).toBeNull();
      expect(state.deck).toHaveLength(20 - Math.min(3, Math.floor((turn + 1) / 4)) * 4);
    }
    expect(seats).toEqual([0, 1, 2, 3, 1, 2, 3, 0, 2, 3, 0, 1, 3, 0, 1, 2]);
    expect(state.hands.map(hand => hand.length)).toEqual([8, 8, 8, 8]);
    expect(new Set([...state.hands.flat(), ...state.offers.filter(Boolean), ...state.deck]).size).toBe(40);
    expect(draftSeat(state)).toBe(null);
    expect(pickDraftCard(state, 0)).toBe(state);
  });

  it('rejects an insufficient deck and ignores invalid picks', () => {
    expect(() => createDraft(cards.slice(0, 31))).toThrow();
    expect(createDraft(cards.slice(0, 32)).deck).toHaveLength(12);
    const state = createDraft(cards);
    for (const index of [-1, 4, 1.5, undefined]) expect(pickDraftCard(state, index)).toBe(state);
    const next = pickDraftCard(state, 0);
    expect(pickDraftCard(next, 0)).toBe(next);
  });

  it('keeps the final remaining card for the fourth player, then opens a fresh round', () => {
    let state = createDraft(cards);
    for (const index of [1, 3, 0]) state = pickDraftCard(state, index);
    expect(state.offers).toEqual([null, null, cards[18], null]);
    expect(state.deck).toEqual(cards.slice(20));
    expect(draftSeat(state)).toBe(3);
    state = pickDraftCard(state, 2);
    expect(state.hands[3].at(-1)).toBe(cards[18]);
    expect(state.offers).toEqual(cards.slice(20, 24));
    expect(draftSeat(state)).toBe(1);
  });

  it('finishes with the minimum 32 cards without drawing extra cards', () => {
    let state = createDraft(cards.slice(0, 32));
    for (let turn = 0; turn < 16; turn++) state = pickDraftCard(state, state.offers.findIndex(Boolean));
    expect(state.hands.map(hand => hand.length)).toEqual([8, 8, 8, 8]);
    expect(state.deck).toEqual([]);
    expect(state.offers).toEqual([null, null, null, null]);
  });
});
