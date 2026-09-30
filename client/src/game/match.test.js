import { describe, expect, it } from 'vitest';
import { eloDelta, expectedScore } from './elo';
import { gameReducer, initGame } from './engine';
import { buildMatchSummary } from './summary';

const person = (id, elo = 1500) => ({ id, name: id, initials: id.slice(0, 2), tint: '#fff', elo });
const LINEUP = [person('me', 1500), person('p1', 1500), person('p2', 1500), person('p3', 1500)];

describe('elo', () => {
  it('expected score is 0.5 for equal ratings', () => {
    expect(expectedScore(1500, 1500)).toBeCloseTo(0.5);
  });

  it('win vs equal gives +16, loss gives -16 (K=32)', () => {
    expect(eloDelta(1500, 1500, true)).toBe(16);
    expect(eloDelta(1500, 1500, false)).toBe(-16);
  });

  it('beating a stronger opponent gives more', () => {
    expect(eloDelta(1400, 1600, true)).toBeGreaterThan(16);
  });
});

describe('initGame with lineup', () => {
  it('uses lineup players in seat order', () => {
    const s = initGame('classic', LINEUP);
    expect(s.players.map((p) => p.id)).toEqual(['me', 'p1', 'p2', 'p3']);
    expect(s.players[0].isHuman).toBe(true);
    expect(s.players[1].isHuman).toBe(false);
  });

  it('2v2: teammate is seat 2', () => {
    const s = initGame('2v2', LINEUP);
    expect(s.players.map((p) => p.team)).toEqual(['A', 'B', 'A', 'B']);
  });

  it('side: teammate is seat 1', () => {
    const s = initGame('side', LINEUP);
    expect(s.players.map((p) => p.team)).toEqual(['A', 'A', 'B', 'B']);
  });

  it('starts every player with zeroed stats', () => {
    const s = initGame('classic', LINEUP);
    s.players.forEach((p) => expect(p.stats).toEqual({ played: 0, attacks: 0, unoCalls: 0, penalties: 0 }));
  });

  it('falls back to bot lineup without argument', () => {
    const s = initGame('classic');
    expect(s.players).toHaveLength(4);
    expect(s.players[0].id).toBe('p-me');
  });
});

/** State nhỏ, kiểm soát được bài trên tay để test reducer. */
function rigged(modeKey, hands, top = { id: 't', color: 'red', value: '5' }) {
  const s = initGame(modeKey, LINEUP);
  return {
    ...s,
    players: s.players.map((p, i) => ({ ...p, hand: hands[i] })),
    discard: [{ ...top, tilt: 0 }],
    currentColor: top.color,
    turn: 0,
  };
}
const c = (id, color, value) => ({ id, color, value });

describe('reducer stats', () => {
  it('counts played cards and attacks', () => {
    const s = rigged('classic', [
      [c('a', 'red', 'draw2'), c('b', 'red', '1'), c('x', 'blue', '3')],
      [c('d', 'blue', '9')],
      [c('e', 'blue', '9')],
      [c('f', 'blue', '9')],
    ]);
    const n = gameReducer(s, { type: 'PLAY', player: 0, cardId: 'a' });
    expect(n.players[0].stats.played).toBe(1);
    expect(n.players[0].stats.attacks).toBe(1);
  });

  it('counts UNO calls and penalties', () => {
    let s = rigged('classic', [
      [c('a', 'red', '1'), c('b', 'red', '2')],
      [c('d', 'blue', '9')],
      [c('e', 'blue', '9')],
      [c('f', 'blue', '9')],
    ]);
    s = gameReducer(s, { type: 'SAY_UNO', player: 0 });
    expect(s.players[0].stats.unoCalls).toBe(1);

    let t = rigged('classic', [
      [c('a', 'red', '1'), c('b', 'red', '2')],
      [c('d', 'blue', '9')],
      [c('e', 'blue', '9')],
      [c('f', 'blue', '9')],
    ]);
    t = gameReducer(t, { type: 'PLAY', player: 0, cardId: 'a' });
    t = gameReducer(t, { type: 'UNO_PENALTY', watchId: t.unoWatch.id });
    expect(t.players[0].stats.penalties).toBe(1);
  });
});

describe('buildMatchSummary', () => {
  const finish = (modeKey, winnerIdx, hands) => {
    const s = rigged(modeKey, hands);
    const w = s.players[winnerIdx];
    return { ...s, startedAt: 0, winner: { player: winnerIdx, team: w.team } };
  };
  const hands = [[], [c('a', 'red', '7'), c('b', 'blue', 'wild4')], [c('d', 'red', '3')], [c('e', 'red', 'skip')]];

  it('classic win: ranked delta, points, duration', () => {
    const sum = buildMatchSummary(finish('classic', 0, hands), { ranked: true, now: 90_000 });
    expect(sum.won).toBe(true);
    expect(sum.durationSec).toBe(90);
    expect(sum.eloBefore).toBe(1500);
    expect(sum.eloDelta).toBe(16);
    expect(sum.players.find((p) => p.id === 'p1').points).toBe(57);
    expect(sum.players.find((p) => p.id === 'p1').cardsLeft).toBe(2);
  });

  it('2v2: teammate winning counts as a win', () => {
    const h = [[c('z', 'red', '1')], [c('a', 'red', '7')], [], [c('e', 'red', 'skip')]];
    const sum = buildMatchSummary(finish('2v2', 2, h), { ranked: true, now: 1000 });
    expect(sum.won).toBe(true);
    expect(sum.winnerTeam).toBe('A');
  });

  it('unranked has null delta', () => {
    const sum = buildMatchSummary(finish('classic', 1, hands), { ranked: false, now: 1000 });
    expect(sum.won).toBe(false);
    expect(sum.eloDelta).toBeNull();
  });
});
