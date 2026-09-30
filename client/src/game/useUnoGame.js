import { useEffect, useMemo, useReducer, useRef } from 'react';
import { chooseBotMove } from './bot';
import { TURN_SECONDS, UNO_GRACE_MS } from './constants';
import { gameReducer, initGame } from './engine';
import { playableCards, topCard } from './rules';

/**
 * Hook quản lý 1 ván UNO chơi local với bot.
 * Khi có server: thay dispatch local bằng gửi action lên socket và nhận state về.
 */
export function useUnoGame(modeKey, lineup = null) {
  const [state, dispatch] = useReducer(gameReducer, null, () => initGame(modeKey, lineup));
  const stateRef = useRef(state);
  stateRef.current = state;

  const turnKey = `${state.turnNo}-${state.hasDrawn}`;
  const current = state.players[state.turn];

  // Lượt của bot
  useEffect(() => {
    if (state.winner || state.pendingWild || current.isHuman) return undefined;
    const delay = state.hasDrawn ? 650 : 900 + Math.random() * 700;
    const t = setTimeout(() => dispatch(chooseBotMove(stateRef.current, state.turn)), delay);
    return () => clearTimeout(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [turnKey, state.winner, state.pendingWild]);

  // Hết giờ lượt của người chơi
  useEffect(() => {
    if (state.winner || !current.isHuman) return undefined;
    const t = setTimeout(
      () => dispatch({ type: 'TIMEOUT', player: state.turn, turnNo: state.turnNo }),
      TURN_SECONDS * 1000,
    );
    return () => clearTimeout(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [turnKey, state.winner]);

  // Còn 1 lá mà chưa hô UNO → bot tự hô (thường là vậy), quá hạn thì phạt
  const watchId = state.unoWatch?.id;
  useEffect(() => {
    const w = stateRef.current.unoWatch;
    if (!w) return undefined;
    const timers = [];
    if (!stateRef.current.players[w.player].isHuman && Math.random() < 0.85) {
      timers.push(setTimeout(() => dispatch({ type: 'SAY_UNO', player: w.player }), 500 + Math.random() * 600));
    }
    timers.push(setTimeout(() => dispatch({ type: 'UNO_PENALTY', watchId: w.id }), UNO_GRACE_MS));
    return () => timers.forEach(clearTimeout);
  }, [watchId]);

  const me = state.players[0];
  const top = topCard(state);
  const isMyTurn = state.turn === 0 && !state.winner;
  const playableIds = useMemo(() => new Set(playableCards(state, 0).map((c) => c.id)), [state]);

  const canSayUno =
    !state.winner &&
    ((state.unoWatch?.player === 0) || (me.hand.length === 2 && isMyTurn && !me.saidUno && playableIds.size > 0));

  const actions = useMemo(
    () => ({
      play: (cardId) => dispatch({ type: 'PLAY', player: 0, cardId }),
      chooseColor: (color) => dispatch({ type: 'CHOOSE_COLOR', color }),
      cancelWild: () => dispatch({ type: 'CANCEL_WILD' }),
      draw: () => dispatch({ type: 'DRAW', player: 0 }),
      pass: () => dispatch({ type: 'PASS', player: 0 }),
      sayUno: () => dispatch({ type: 'SAY_UNO', player: 0 }),
      chat: (text) => dispatch({ type: 'CHAT', player: 0, text }),
      restart: () => dispatch({ type: 'RESET', state: initGame(modeKey, lineup) }),
    }),
    [modeKey, lineup],
  );

  return { state, me, top, isMyTurn, playableIds, canSayUno, turnKey, actions };
}
