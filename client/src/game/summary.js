import { DEFAULT_ELO, avgElo, eloDelta } from './elo';

const POINTS = { skip: 20, reverse: 20, draw2: 20, discard_all: 20, wild: 50, wild4: 50 };
export const handPoints = (hand) => hand.reduce((sum, c) => sum + (POINTS[c.value] ?? Number(c.value)), 0);

/** Tóm tắt 1 ván đã kết thúc -> dữ liệu cho trang tổng kết. */
export function buildMatchSummary(state, { ranked, now = Date.now() }) {
  const me = state.players[0];
  const winnerP = state.players[state.winner.player];
  const won = state.teams ? winnerP.team === me.team : state.winner.player === 0;

  const players = state.players.map((p) => ({
    id: p.id,
    name: p.name,
    initials: p.initials,
    tint: p.tint,
    elo: p.elo,
    isHuman: p.isHuman,
    isBot: !!p.isBot,
    botLevel: p.botLevel,
    team: p.team,
    cardsLeft: p.hand.length,
    points: handPoints(p.hand),
    stats: p.stats,
    isWinner: p.id === winnerP.id,
  }));

  let delta = null;
  if (ranked) {
    const mine = state.teams ? players.filter((p) => p.team === me.team) : [players[0]];
    const theirs = state.teams ? players.filter((p) => p.team !== me.team) : players.slice(1);
    delta = eloDelta(avgElo(mine), avgElo(theirs), won);
  }

  return {
    mode: state.mode,
    ranked,
    teams: state.teams,
    won,
    winnerId: winnerP.id,
    winnerTeam: winnerP.team,
    myTeam: me.team,
    durationSec: Math.round((now - state.startedAt) / 1000),
    players,
    eloBefore: me.elo ?? DEFAULT_ELO,
    eloDelta: delta,
  };
}
