import { expect, it } from 'vitest';
import { RANKS, rankOf } from './ranks';

it('assigns every ELO threshold to exactly one distinct badge', () => {
  expect(new Set(RANKS.map(r => r.key)).size).toBe(6);
  RANKS.forEach((rank, i) => {
    expect(rankOf(rank.min)).toBe(rank);
    if (i) expect(rankOf(rank.min - 1)).toBe(RANKS[i - 1]);
  });
  expect(rankOf(-1)).toBe(RANKS[0]);
  expect(rankOf(5000)).toBe(RANKS.at(-1));
});
