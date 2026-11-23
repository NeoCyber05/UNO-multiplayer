export const initialSession = {
  me: null, connection: 'connecting', room: null, party: null, queue: null,
  game: null, receivedAt: 0, result: null, friends: [], leaderboard: [], profile: null,
  invites: [], chat: null,
};

export const stateEvents = [
  'CONNECTION_STATE', 'SERVER_SHUTDOWN', 'ROOM_STATE', 'PARTY_STATE', 'QUEUE_STATE',
  'GAME_STATE', 'MATCH_RESULT', 'GAME_CHAT', 'FRIEND_STATE', 'LEADERBOARD_STATE',
  'PROFILE_STATE', 'LOBBY_STATE', 'INVITE',
];

function newer(current, next, id) {
  return !current || current[id] !== next[id] || next.revision == null || next.revision > current.revision;
}

export function sessionReducer(state, { type, payload, receivedAt }) {
  switch (type) {
    case 'CONNECTION_STATE':
      return {
        ...state,
        ...(payload.status === 'disconnected' && { room: null, queue: null, party: null, invites: [] }),
        connection: payload.status,
      };
    case 'SERVER_SHUTDOWN':
      return { ...state, connection: 'disconnected' };
    case 'AUTH_OK':
      return { ...state, me: payload, connection: 'connected' };
    case 'AUTH_CLEAR':
      return { ...initialSession, connection: state.connection };
    case 'ROOM_STATE':
      return !payload || newer(state.room, payload, 'roomId') ? { ...state, room: payload } : state;
    case 'PARTY_STATE':
      return !payload || newer(state.party, payload, 'partyId') ? { ...state, party: payload } : state;
    case 'QUEUE_STATE':
      return !payload || newer(state.queue, payload, 'mode') ? { ...state, queue: payload } : state;
    case 'GAME_STATE':
      if (!payload || newer(state.game, payload, 'matchId')) {
        const newMatch = payload?.matchId !== state.game?.matchId;
        return {
          ...state, game: payload, receivedAt, chat: newMatch ? null : state.chat,
          result: newMatch && payload ? null : state.result,
        };
      }
      return state;
    case 'GAME_CHAT':
      return { ...state, chat: { ...payload, receivedAt } };
    case 'MATCH_RESULT': {
      if (payload.matchId !== state.game?.matchId) return state;
      const viewerSeat = state.game.viewerSeat;
      const player = payload.players.find((item) => item.seat === viewerSeat);
      const me = state.me && player ? { ...state.me, elo: player.eloAfter } : state.me;
      return { ...state, me, result: { ...payload, viewerSeat }, room: null, queue: null };
    }
    case 'FRIEND_STATE':
      return { ...state, friends: payload.friends ?? [] };
    case 'LEADERBOARD_STATE':
      return { ...state, leaderboard: payload.entries ?? [] };
    case 'PROFILE_STATE':
      return {
        ...state,
        profile: payload.userId === state.profile?.userId ? { ...state.profile, ...payload } : payload,
        me: payload.userId === state.me?.userId ? { ...state.me, ...payload } : state.me,
      };
    case 'LOBBY_STATE':
      return {
        ...state, friends: payload.friends ?? state.friends, leaderboard: payload.leaderboard ?? state.leaderboard,
      };
    case 'INVITE':
      return { ...state, invites: [...state.invites.filter((invite) => invite.inviteId !== payload.inviteId), payload] };
    case 'INVITE_DONE':
      return { ...state, invites: state.invites.filter((invite) => invite.inviteId !== payload) };
    default:
      return state;
  }
}
