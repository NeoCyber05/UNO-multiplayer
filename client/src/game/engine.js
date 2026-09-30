import { BOTS, ME } from '../data/mockData';
import { COLOR_LABEL, HAND_SIZE, MODES } from './constants';
import { createDeck, shuffle } from './deck';
import { canPlay, isWild, nextIndex, topCard } from './rules';

const OUCH = ['Ối trời!', 'Không thể nào 😭', 'Nhớ đấy nhé!', 'Ác quá vậy 😤'];
const pick = (arr) => arr[Math.floor(Math.random() * arr.length)];

function tiltOf(id) {
  let h = 0;
  for (const ch of id) h = (h * 31 + ch.charCodeAt(0)) | 0;
  return (Math.abs(h) % 36) - 18;
}

const emptyStats = () => ({ played: 0, attacks: 0, unoCalls: 0, penalties: 0 });

/** lineup: 4 người theo ghế (0 = bạn, 1 = trái, 2 = trên, 3 = phải). Không có -> đội hình bot mặc định. */
function buildPlayers(modeKey, lineup) {
  const cfg = MODES[modeKey];
  const [ma, hk, tn] = BOTS;
  const people = lineup ?? [ME, ...(modeKey === 'side' ? [ma, tn, hk] : [ma, hk, tn])];
  return people.map((p, i) => ({
    ...p,
    isHuman: i === 0,
    team: cfg.teams ? (i === 0 || i === cfg.teammateSeat ? 'A' : 'B') : p.id,
    hand: [],
    saidUno: false,
    stats: emptyStats(),
  }));
}

export function initGame(modeKey, lineup = null) {
  const cfg = MODES[modeKey] ?? MODES.classic;
  const deck = shuffle(createDeck());
  const players = buildPlayers(cfg.key, lineup);
  for (let r = 0; r < HAND_SIZE; r++) players.forEach((p) => p.hand.push(deck.pop()));
  const firstIdx = deck.findIndex((c) => /^\d$/.test(c.value));
  const [first] = deck.splice(firstIdx, 1);

  return {
    mode: cfg.key,
    teams: cfg.teams,
    players,
    drawPile: deck,
    discard: [{ ...first, tilt: 0 }],
    currentColor: first.color,
    turn: 0,
    turnNo: 1,
    direction: 1,
    hasDrawn: false,
    drawnCardId: null,
    pendingWild: null,
    unoWatch: null,
    winner: null,
    bubbles: {},
    message: { id: 1, text: 'Ván đấu bắt đầu! Bạn đi trước.' },
    seq: 1,
    startedAt: Date.now(),
  };
}

/* ---------- helpers (thuần, trả về state mới) ---------- */

const displayName = (p) => (p.isHuman ? 'Bạn' : p.name);

function patchPlayer(players, idx, patch) {
  return players.map((p, i) => (i === idx ? { ...p, ...patch } : p));
}

/** Cộng dồn chỉ số của người chơi idx, vd bumpStats(p, { played: 1 }). */
function bumpStats(p, add) {
  const stats = { ...p.stats };
  for (const [k, v] of Object.entries(add)) stats[k] = (stats[k] ?? 0) + v;
  return stats;
}

/** Chia n lá cho người chơi idx (tự xào lại chồng bài đã đánh khi hết bài rút). */
function giveCards(s, idx, n) {
  let drawPile = [...s.drawPile];
  let discard = s.discard;
  const drawn = [];
  for (let i = 0; i < n; i++) {
    if (!drawPile.length) {
      if (discard.length <= 1) break;
      const top = discard[discard.length - 1];
      // eslint-disable-next-line no-unused-vars
      drawPile = shuffle(discard.slice(0, -1).map(({ tilt, chosenColor, ...card }) => card));
      discard = [top];
    }
    drawn.push(drawPile.pop());
  }
  const p = s.players[idx];
  return {
    state: { ...s, drawPile, discard, players: patchPlayer(s.players, idx, { hand: [...p.hand, ...drawn], saidUno: false }) },
    drawn,
  };
}

function withMessage(s, text) {
  const id = s.seq + 1;
  return { ...s, seq: id, message: { id, text } };
}

function withBubble(s, idx, text) {
  const id = s.seq + 1;
  return { ...s, seq: id, bubbles: { ...s.bubbles, [s.players[idx].id]: { id, text } } };
}

function advanceFrom(s, from, steps) {
  return { ...s, turn: nextIndex(s, from, steps), turnNo: s.turnNo + 1, hasDrawn: false, drawnCardId: null };
}

function applyPlay(s, idx, card, chosenColor) {
  const p = s.players[idx];
  const color = isWild(card) ? chosenColor : card.color;
  // Discard All: bỏ luôn mọi lá cùng màu trên tay (xuống dưới lá Discard All, không kích hoạt hiệu ứng)
  const dumped = card.value === 'discard_all' ? p.hand.filter((c) => c.id !== card.id && c.color === card.color) : [];
  const hand = p.hand.filter((c) => c.id !== card.id && !dumped.includes(c));

  let n = {
    ...s,
    players: patchPlayer(s.players, idx, {
      hand,
      saidUno: hand.length <= 1 ? p.saidUno : false,
      stats: bumpStats(p, {
        played: 1 + dumped.length,
        attacks: card.value === 'draw2' || card.value === 'wild4' ? 1 : 0,
      }),
    }),
    discard: [
      ...s.discard,
      ...dumped.map((c) => ({ ...c, tilt: tiltOf(c.id) })),
      { ...card, tilt: tiltOf(card.id), chosenColor: isWild(card) ? chosenColor : undefined },
    ],
    currentColor: color,
    pendingWild: null,
    unoWatch: null,
    hasDrawn: false,
    drawnCardId: null,
  };

  if (!hand.length) {
    n = withMessage(n, p.isHuman ? 'Bạn đã hết bài!' : `${p.name} đã hết bài!`);
    return { ...n, winner: { player: idx, team: p.team } };
  }

  if (hand.length === 1 && !p.saidUno) {
    n = { ...n, unoWatch: { player: idx, id: n.seq + 1 } };
  }

  const target = nextIndex(n, idx);
  const targetP = n.players[target];

  switch (card.value) {
    case 'skip':
      n = withMessage(n, `${displayName(targetP)} bị mất lượt!`);
      return advanceFrom(n, idx, 2);
    case 'reverse': {
      n = withMessage({ ...n, direction: -n.direction }, 'Đảo chiều!');
      return advanceFrom(n, idx, 1);
    }
    case 'draw2':
    case 'wild4': {
      const count = card.value === 'draw2' ? 2 : 4;
      n = giveCards(n, target, count).state;
      const colorNote = card.value === 'wild4' ? ` · Màu ${COLOR_LABEL[color]}` : '';
      n = withMessage(n, `${displayName(targetP)} +${count} lá & mất lượt${colorNote}`);
      if (!targetP.isHuman) n = withBubble(n, target, pick(OUCH));
      if (!p.isHuman && card.value === 'wild4') n = withBubble(n, idx, 'Hehe 😎');
      return advanceFrom(n, idx, 2);
    }
    case 'wild':
      n = withMessage(n, `${displayName(p)} đổi màu: ${COLOR_LABEL[color]}`);
      return advanceFrom(n, idx, 1);
    case 'discard_all':
      n = withMessage(n, `${displayName(p)} bỏ hết ${dumped.length + 1} lá màu ${COLOR_LABEL[color]}!`);
      return advanceFrom(n, idx, 1);
    default:
      return advanceFrom(n, idx, 1);
  }
}

/* ---------- reducer ---------- */

export function gameReducer(s, a) {
  switch (a.type) {
    case 'RESET':
      return a.state;

    case 'PLAY': {
      if (s.winner || s.pendingWild || a.player !== s.turn) return s;
      const card = s.players[a.player].hand.find((c) => c.id === a.cardId);
      if (!card) return s;
      if (s.hasDrawn && card.id !== s.drawnCardId) return s;
      if (!canPlay(card, topCard(s), s.currentColor)) return s;
      if (isWild(card) && !a.color) return { ...s, pendingWild: { player: a.player, cardId: card.id } };
      return applyPlay(s, a.player, card, a.color);
    }

    case 'CHOOSE_COLOR': {
      if (!s.pendingWild) return s;
      const { player, cardId } = s.pendingWild;
      const card = s.players[player].hand.find((c) => c.id === cardId);
      if (!card) return { ...s, pendingWild: null };
      return applyPlay(s, player, card, a.color);
    }

    case 'CANCEL_WILD':
      return { ...s, pendingWild: null };

    case 'DRAW': {
      if (s.winner || s.pendingWild || a.player !== s.turn || s.hasDrawn) return s;
      const { state: n, drawn } = giveCards(s, a.player, 1);
      const card = drawn[0];
      if (card && canPlay(card, topCard(n), n.currentColor)) {
        return { ...n, hasDrawn: true, drawnCardId: card.id };
      }
      return advanceFrom(withMessage(n, `${displayName(s.players[a.player])} rút 1 lá`), a.player, 1);
    }

    case 'PASS':
      if (s.winner || a.player !== s.turn || !s.hasDrawn) return s;
      return advanceFrom(s, a.player, 1);

    case 'TIMEOUT': {
      if (s.winner || a.player !== s.turn || a.turnNo !== s.turnNo) return s;
      let n = { ...s, pendingWild: null };
      if (!n.hasDrawn) n = giveCards(n, a.player, 1).state;
      n = withMessage(n, 'Hết giờ! Tự động rút bài & bỏ lượt');
      return advanceFrom(n, a.player, 1);
    }

    case 'SAY_UNO': {
      const p = s.players[a.player];
      if (!p || s.winner) return s;
      const watched = s.unoWatch?.player === a.player;
      const early = p.hand.length === 2 && s.turn === a.player && !p.saidUno;
      if (!watched && !early) return s;
      const n = {
        ...s,
        players: patchPlayer(s.players, a.player, { saidUno: true, stats: bumpStats(p, { unoCalls: 1 }) }),
        unoWatch: watched ? null : s.unoWatch,
      };
      return withBubble(n, a.player, 'UNO!');
    }

    case 'UNO_PENALTY': {
      if (!s.unoWatch || s.unoWatch.id !== a.watchId || s.winner) return s;
      const idx = s.unoWatch.player;
      const who = s.players[idx];
      const penalized = { ...s, unoWatch: null, players: patchPlayer(s.players, idx, { stats: bumpStats(who, { penalties: 1 }) }) };
      const n = giveCards(penalized, idx, 2).state;
      return withMessage(n, who.isHuman ? 'Bạn quên hô UNO! Phạt +2 lá' : `${who.name} quên hô UNO! Phạt +2 lá`);
    }

    case 'CHAT':
      return withBubble(s, a.player, a.text);

    default:
      return s;
  }
}
