import { describe, expect, it } from 'vitest';
import snapshots from '../../../tests/fixtures/views.json';
import { toMatchSummary, toTableView, turnPayload } from './view';

describe('wire table view', () => {
  it.each(snapshots)('rotates $mode without renumbering seats or teams', (snapshot) => {
    const table = toTableView(snapshot, 42);
    expect(table.players[0].seat).toBe(snapshot.viewerSeat);
    expect(table.players[table.turn].seat).toBe(snapshot.turn);
    for (const player of table.players) {
      expect(player.team).toBe(snapshot.players[player.seat].team);
      expect(player.hand).toHaveLength(player.cardCount);
      if (!snapshot.players[player.seat].hand) {
        expect(player.hand.every((card) => !card.color && !card.value)).toBe(true);
      }
    }
    expect(table.discard).toEqual([snapshot.topCard]);
    expect(turnPayload(snapshot, { cardId: 'c17' })).toEqual({
      matchId: snapshot.matchId, turnNo: snapshot.turnNo, cardId: 'c17',
    });
  });

  it('uses the finished result for the viewer and their ELO', () => {
    const result = {
      mode: '2v2', winnerSeat: 0, winnerTeam: 0, durationMs: 57500, ranked: true,
      players: [0, 1, 2, 3].map((seat) => ({
        seat, userId: String(seat + 10), team: seat % 2,
        cardsLeft: seat ? 3 : 0, eloBefore: 1500, eloAfter: seat % 2 ? 1483 : 1517,
      })),
    };
    const summary = toMatchSummary(result, 2);
    expect(summary).toMatchObject({ won: true, eloDelta: 17, winnerId: '10', durationSec: 57 });
    expect(summary.players.filter((player) => player.isHuman).map((player) => player.seat)).toEqual([2]);
  });
});
