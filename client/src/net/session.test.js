import { expect, it } from 'vitest';
import { initialSession, sessionReducer } from './sessionState';
import { RANKS, rankOf } from '../data/ranks';

it('accepts only newer state for each room and game', () => {
  const room = { roomId: 'ROOM01', code: 'ROOM01', revision: 2, hostSeat: 1, seats: [] };
  const game = { matchId: 'match', revision: 3 };
  const messages = [
    { type: 'ROOM_STATE', payload: room },
    { type: 'ROOM_STATE', payload: { ...room, revision: 1, hostSeat: 0 } },
    { type: 'GAME_STATE', payload: game },
    { type: 'GAME_STATE', payload: { ...game, turn: 3 } },
  ];
  const session = messages.reduce(sessionReducer, initialSession);
  expect(session.room).toEqual(room);
  expect(session.game).toEqual(game);
});

it('uses server auth, connection, profile and queue updates', () => {
  expect(sessionReducer(initialSession, { type: 'ERROR' }).me).toBeNull();
  const session = [
    { type: 'AUTH_OK', payload: { userId: '1', name: 'Bách' } },
    { type: 'QUEUE_STATE', payload: { mode: 'side', revision: 1 } },
    { type: 'PROFILE_STATE', payload: { userId: '1', name: 'Tên mới' } },
    { type: 'CONNECTION_STATE', payload: { status: 'disconnected' } },
  ].reduce(sessionReducer, initialSession);
  expect(session.me.name).toBe('Tên mới');
  expect(session.connection).toBe('disconnected');
  expect(session.queue).toBeNull();
  expect(sessionReducer(initialSession, {
    type: 'QUEUE_STATE', payload: { mode: 'side', revision: 1 },
  }).queue.mode).toBe('side');
  expect(session.leaderboard).toEqual([]);
  expect(RANKS.map((rank) => rank.key)).toEqual(['bronze', 'silver', 'gold', 'diamond']);
  expect(rankOf(1799).key).toBe('gold');
  expect(rankOf(1800).key).toBe('diamond');
});
