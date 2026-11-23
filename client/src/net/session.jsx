import { createContext, useCallback, useContext, useEffect, useMemo, useReducer } from 'react';
import { createBridge } from './bridge';
import { initialSession, sessionReducer, stateEvents } from './sessionState';

const SessionContext = createContext(null);

export function SessionProvider({ children }) {
  const bridge = useMemo(() => createBridge(window), []);
  const [state, dispatch] = useReducer(sessionReducer, initialSession);
  const receive = useCallback((type, payload) => {
    dispatch({ type, payload, receivedAt: performance.now() });
  }, []);

  useEffect(() => {
    const unsubscribe = stateEvents.map((type) => bridge.subscribe(type, (payload) => {
      receive(type, payload);
      if (type === 'CONNECTION_STATE' && payload?.me) receive('AUTH_OK', payload.me);
      if (type === 'CONNECTION_STATE' && payload?.sessionExpired) receive('AUTH_CLEAR');
    }));
    bridge.start();
    let disposed = false;
    if (bridge.available()) {
      bridge.request('HELLO').then(() => {
        if (!disposed) receive('CONNECTION_STATE', { status: 'connected' });
      }).catch(() => {
        if (!disposed) receive('CONNECTION_STATE', { status: 'disconnected' });
      });
    } else {
      receive('CONNECTION_STATE', { status: 'unavailable' });
    }
    return () => {
      disposed = true;
      unsubscribe.forEach((stop) => stop());
      bridge.stop();
    };
  }, [bridge, receive]);

  const userId = state.me?.userId;
  const connection = state.connection;
  useEffect(() => {
    if (!userId || connection !== 'connected') return;
    let disposed = false;
    bridge.request('FRIEND_LIST').then((payload) => {
      if (!disposed && Array.isArray(payload?.friends)) receive('FRIEND_STATE', payload);
    }).catch(() => {
      // The friends drawer handles errors from an explicit refresh.
    });
    return () => { disposed = true; };
  }, [bridge, connection, receive, userId]);

  const request = useCallback(async (type, payload) => {
    let response;
    try {
      response = await bridge.request(type, payload);
    } catch (error) {
      if (type === 'INVITE_REPLY' && error.code === 'NOT_FOUND') {
        receive('INVITE_DONE', payload.inviteId);
      }
      throw error;
    }
    if (type === 'LOGIN' && response.me) receive('AUTH_OK', response.me);
    if (type === 'LOGOUT') receive('AUTH_CLEAR');
    if (type === 'ROOM_LEAVE') receive('ROOM_STATE', null);
    if (type === 'PARTY_LEAVE') receive('PARTY_STATE', null);
    if (type === 'QUEUE_LEAVE') receive('QUEUE_STATE', null);
    if (type === 'MATCH_LEAVE') {
      receive('ROOM_STATE', null);
      receive('GAME_STATE', null);
    }
    if (type === 'INVITE_REPLY') receive('INVITE_DONE', payload.inviteId);
    if (type === 'PROFILE_UPDATE' && response?.profile) receive('PROFILE_STATE', response.profile);
    return response;
  }, [bridge, receive]);

  return <SessionContext.Provider value={{ ...state, request, receive }}>{children}</SessionContext.Provider>;
}

export function useSession() {
  return useContext(SessionContext);
}
