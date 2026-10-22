// Elo chuẩn. Sau này tính ở server.
export const ELO_K = 32;
export const DEFAULT_ELO = 1500;

export const expectedScore = (mine, theirs) => 1 / (1 + 10 ** ((theirs - mine) / 400));

export function eloDelta(mine, theirs, won, k = ELO_K) {
  return Math.round(k * ((won ? 1 : 0) - expectedScore(mine, theirs)));
}

export const avgElo = (people) => people.reduce((sum, p) => sum + (p.elo ?? DEFAULT_ELO), 0) / people.length;
