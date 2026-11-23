export function toTableView(snapshot, receivedAt) {
  const relative = (seat) => (seat - snapshot.viewerSeat + 4) % 4;
  const players = [];
  for (const player of snapshot.players) {
    const index = relative(player.seat);
    players[index] = {
      ...player,
      id: player.isBot ? `bot-${player.seat}` : player.userId,
      isHuman: index === 0,
      unoCalls: index === 0 ? snapshot.unoCalls : undefined,
      handVisible: Array.isArray(player.hand),
      hand: player.hand ?? Array.from({ length: player.cardCount }, (_, slot) => ({
        id: `hidden-${player.seat}-${slot}`,
      })),
    };
  }
  return {
    players,
    turn: relative(snapshot.turn),
    draft: snapshot.phase === 'DRAFT' ? { turn: snapshot.draftTurn, offers: snapshot.offers } : null,
    winner: snapshot.winner,
    discard: [snapshot.topCard],
    drawCount: snapshot.drawCount,
    currentColor: snapshot.currentColor,
    direction: snapshot.direction,
    hasDrawn: snapshot.canPass,
    drawnCardId: snapshot.canPass ? snapshot.legalCardIds[0] : null,
    durationMs: snapshot.durationMs,
    localDeadline: receivedAt + snapshot.deadline - snapshot.serverNowMs,
    receivedAt,
    paused: snapshot.paused,
    viewerSeat: snapshot.viewerSeat,
  };
}

export function turnPayload(snapshot, payload = {}) {
  return { matchId: snapshot.matchId, turnNo: snapshot.turnNo, ...payload };
}

export function toMatchSummary(result, viewerSeat) {
  const teams = result.mode === '2v2' || result.mode === 'side';
  const players = result.players.map((player) => ({
    ...player,
    id: player.isBot ? `bot-${player.seat}` : player.userId,
    isHuman: player.seat === viewerSeat,
    isWinner: teams ? player.team === result.winnerTeam : player.seat === result.winnerSeat,
  }));
  const me = players.find((player) => player.isHuman);
  return {
    ...result,
    players,
    teams,
    myTeam: me.team,
    won: me.isWinner,
    winnerId: players.find((player) => player.seat === result.winnerSeat)?.id,
    durationSec: Math.floor(result.durationMs / 1000),
    eloBefore: me.eloBefore,
    eloDelta: result.ranked ? me.eloAfter - me.eloBefore : null,
  };
}
