export const initialSession = {
  me: null, connection: 'connecting', room: null, party: null, queue: null,
  game: null, receivedAt: 0, result: null, friends: [], leaderboard: [], profile: null,
  invites: [], chat: null,
};

export const stateEvents = [
  'CONNECTION_STATE', 'SERVER_SHUTDOWN', 'ROOM_STATE', 'PARTY_STATE', 'QUEUE_STATE',
  'GAME_STATE', 'MATCH_RESULT', 'GAME_CHAT', 'FRIEND_STATE', 'PROFILE_STATE', 'INVITE',
];

function newer(current, next, id) {
  return !current || current[id] !== next[id] || next.revision == null || next.revision > current.revision;
}

export function sessionReducer(state, { type, payload, receivedAt }) {
  switch (type) {
    case 'CONNECTION_STATE': {
      const { status } = payload;
      const reset = status === 'reconnecting' || status === 'disconnected';
      return {
        ...state,
        ...(reset && { room: null, queue: null, party: null, invites: [], game: null, receivedAt: 0, chat: null }),
        connection: status,
      };
    }
    case 'SERVER_SHUTDOWN':
      return { ...state, connection: 'disconnected' };
    case 'AUTH_OK':
      return { ...state, me: payload, connection: 'connected' };
    case 'AUTH_CLEAR':
      return { ...initialSession, connection: state.connection };
    case 'ROOM_STATE': {
      if (payload && !newer(state.room, payload, 'roomId')) return state;
      const seated = state.me && payload?.seats.some((seat) => seat?.userId === state.me.userId);
      return { ...state, room: seated ? payload : null };
    }
    case 'PARTY_STATE':
      if (payload && !newer(state.party, payload, 'partyId')) return state;
      return { ...state, party: payload?.members.length ? payload : null };
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
    case 'INVITE':
      return { ...state, invites: [...state.invites.filter((invite) => invite.inviteId !== payload.inviteId), payload] };
    case 'INVITE_DONE':
      return { ...state, invites: state.invites.filter((invite) => invite.inviteId !== payload) };
    default:
      return state;
  }
}
