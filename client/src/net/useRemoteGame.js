import { useEffect, useMemo, useRef, useState } from 'react';
import { QUICK_CHAT } from '../data/mockData';
import { useSession } from './session';
import { toTableView, turnPayload } from './view';

export function useRemoteGame() {
  const session = useSession();
  const { game: snapshot, receivedAt, connection, request, chat } = session;
  const table = useMemo(() => snapshot ? toTableView(snapshot, receivedAt) : null, [snapshot, receivedAt]);
  const [wild, setWild] = useState(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState('');
  const inFlight = useRef(false);
  const turnKey = snapshot && `${snapshot.matchId}:${snapshot.phase}:${snapshot.draftTurn ?? snapshot.turnNo}`;
  const pendingWild = wild?.turnKey === turnKey ? wild : null;
  const playableIds = new Set(snapshot?.legalCardIds ?? []);
  const isMyTurn = snapshot && snapshot.phase !== 'FINISHED' && snapshot.turn === snapshot.viewerSeat;
  const canAct = connection === 'connected' && !pending;
  const [bubble, setBubble] = useState(null);

  useEffect(() => {
    if (!chat || chat.matchId !== snapshot?.matchId) return;
    setBubble(chat);
    const timer = setTimeout(() => setBubble(null), 3000);
    return () => clearTimeout(timer);
  }, [chat, snapshot?.matchId]);

  async function send(type, payload) {
    if (!snapshot || inFlight.current || connection !== 'connected') return false;
    inFlight.current = true;
    setPending(true);
    setError('');
    try {
      await request(type, payload);
      return true;
    } catch (error) {
      setError(error.message);
      return false;
    } finally {
      inFlight.current = false;
      setPending(false);
    }
  }

  if (!snapshot || !table) return { state: null, connection };
  const me = table.players[0];
  const actions = {
    play(cardId) {
      if (!isMyTurn || !canAct || !playableIds.has(cardId)) return;
      if (me.hand.find((card) => card.id === cardId)?.color === 'wild') {
        setWild({ cardId, turnKey });
      } else {
        send('PLAY', turnPayload(snapshot, { cardId }));
      }
    },
    chooseColor(color) {
      if (!pendingWild || !isMyTurn || !canAct) return;
      setWild(null);
      send('PLAY', turnPayload(snapshot, { cardId: pendingWild.cardId, color }));
    },
    cancelWild: () => setWild(null),
    draw: () => snapshot.canDraw && send('DRAW', turnPayload(snapshot)),
    pass: () => snapshot.canPass && send('PASS', turnPayload(snapshot)),
    sayUno: () => snapshot.canSayUno && send('SAY_UNO', { matchId: snapshot.matchId }),
    draftPick(index) {
      if (!isMyTurn || !canAct || !snapshot.offers[index]) return;
      send('DRAFT_PICK', { matchId: snapshot.matchId, draftTurn: snapshot.draftTurn, index });
    },
    chat: (text) => send('CHAT', { matchId: snapshot.matchId, chatId: QUICK_CHAT.indexOf(text) }),
    leave: () => send('MATCH_LEAVE', { matchId: snapshot.matchId }),
  };
  const speaker = table.players.find((player) => player.seat === bubble?.seat);
  const bubbles = speaker && bubble.matchId === snapshot.matchId
    ? { [speaker.id]: { id: bubble.receivedAt, text: QUICK_CHAT[bubble.chatId] } }
    : {};
  return {
    state: { ...table, pendingWild, bubbles },
    me, isMyTurn, canAct, playableIds, turnKey, actions, connection, error,
    mode: snapshot.mode, matchId: snapshot.matchId, result: session.result,
    canSayUno: snapshot.canSayUno && canAct,
  };
}
