import { describe, it, expect } from 'vitest';
import { initGame, gameReducer } from './engine';
import { draftSeat } from './draft';
import { playableCards } from './rules';

describe('Draft in the game engine', () => {
  it('blocks gameplay, rotates four shared rounds and starts with eight cards each', () => {
    let state = initGame('draft');
    expect(state.mode).toBe('draft');
    expect(state.startedAt).toBeNull();
    expect(state.players.map(p => p.hand.length)).toEqual([4,4,4,4]);
    const ids = s => [...s.players.flatMap(p => p.hand), ...s.drawPile, ...s.discard, ...(s.draft?.offers.filter(Boolean) ?? [])].map(c => c.id);
    const original = ids(state).sort();
    expect(playableCards(state, 0)).toEqual([]);
    for (const type of ['PLAY','DRAW','PASS','TIMEOUT','SAY_UNO']) expect(gameReducer(state, {type, player:0, turnNo:1})).toBe(state);
    for (let turn = 0; turn < 16; turn++) {
      expect(state.draft.offers.filter(Boolean)).toHaveLength(4 - turn % 4);
      const player = draftSeat(state.draft);
      const index = state.draft.offers.findIndex(Boolean);
      expect(gameReducer(state, {type:'DRAFT_PICK', player:(player+1)%4, draftTurn:turn, index})).toBe(state);
      expect(gameReducer(state, {type:'DRAFT_PICK', player, draftTurn:turn-1, index})).toBe(state);
      state = gameReducer(state, {type:'DRAFT_PICK', player, draftTurn:turn, index});
      expect(ids(state).sort()).toEqual(original);
      expect(new Set(ids(state)).size).toBe(original.length);
    }
    expect(state.draft).toBeNull();
    expect(state.players.map(p => p.hand.length)).toEqual([8,8,8,8]);
    expect(state.startedAt).toBeGreaterThan(0);
    expect(state.turn).toBe(0);
    expect(gameReducer(state, {type:'DRAFT_PICK', player:0, draftTurn:16, index:0})).toBe(state);
    expect(gameReducer(state, {type:'DRAW', player:0})).not.toBe(state);
  });
  it('keeps the seven-card opening for existing modes', () => {
    for (const mode of ['classic','2v2','side']) {
      const state = initGame(mode);
      expect(state.players.every(p => p.hand.length === 7)).toBe(true);
      expect(state.draft).toBeNull();
    }
  });
});
