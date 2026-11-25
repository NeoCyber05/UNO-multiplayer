import { expect, it } from 'vitest';
import { initialSession, sessionReducer } from './sessionState';
import { RANKS, rankOf } from '../data/ranks';

it('accepts only newer state for each room and game', () => {
  const room = { roomId: 'ROOM01', code: 'ROOM01', revision: 2, hostSeat: 1, seats: [{ userId: '1' }] };
  const game = { matchId: 'match', revision: 3 };
  const messages = [
    { type: 'AUTH_OK', payload: { userId: '1' } },
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
    { type: 'QUEUE_STATE', payload: { mode: 'side', active: true, revision: 1 } },
    { type: 'PROFILE_STATE', payload: { userId: '1', name: 'Tên mới' } },
    { type: 'CONNECTION_STATE', payload: { status: 'disconnected' } },
  ].reduce(sessionReducer, initialSession);
  expect(session.me.name).toBe('Tên mới');
  expect(session.connection).toBe('disconnected');
  expect(session.queue).toBeNull();
  expect(sessionReducer(initialSession, {
    type: 'QUEUE_STATE', payload: { mode: 'side', active: true, revision: 1 },
  }).queue.mode).toBe('side');
  expect(session.leaderboard).toEqual([]);
  expect(RANKS.map((rank) => rank.key)).toEqual(['bronze', 'silver', 'gold', 'diamond']);
  expect(rankOf(1799).key).toBe('gold');
  expect(rankOf(1800).key).toBe('diamond');
});

it('clears stale membership and game state before applying RESUME snapshots', () => {
  const me = { userId: '1' };
  const game = { matchId: 'match', revision: 3 };
  const before = {
    ...initialSession, me, connection: 'connected', game, receivedAt: 500,
    room: { roomId: 'room' }, party: { partyId: 'party' },
    queue: { mode: 'classic', active: true }, invites: [{ inviteId: 'invite' }], chat: {},
  };
  const offline = sessionReducer(before, { type: 'CONNECTION_STATE', payload: { status: 'reconnecting' } });
  expect(offline).toMatchObject({
    me, connection: 'reconnecting', room: null, party: null, queue: null,
    game: null, invites: [], chat: null,
  });
  const resumed = [
    { type: 'AUTH_OK', payload: me },
    { type: 'GAME_STATE', payload: game, receivedAt: 1500 },
  ].reduce(sessionReducer, offline);
  expect(resumed).toMatchObject({ connection: 'connected', game, receivedAt: 1500, room: null });
});

it('drops membership when the server dissolves the party or removes our seat', () => {
  const room = { roomId: 'room', revision: 2, seats: [{ userId: '1' }, { userId: '2' }] };
  const party = { partyId: 'party', revision: 2, members: ['1', '2'] };
  const before = { ...initialSession, me: { userId: '1' }, room, party };
  const kicked = { ...room, revision: 3, seats: [null, { userId: '2' }] };
  const dissolved = { ...party, revision: 3, leaderId: '0', members: [] };
  expect(sessionReducer(before, { type: 'ROOM_STATE', payload: { ...kicked, revision: 1 } })).toBe(before);
  expect(sessionReducer(before, { type: 'PARTY_STATE', payload: { ...dissolved, revision: 1 } })).toBe(before);
  expect(sessionReducer(before, { type: 'ROOM_STATE', payload: kicked }).room).toBeNull();
  expect(sessionReducer(before, { type: 'PARTY_STATE', payload: dissolved }).party).toBeNull();
});
