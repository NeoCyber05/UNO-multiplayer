import { COLORS } from './constants';

let uid = 0;
const makeCard = (color, value) => ({ id: `c${++uid}`, color, value });

/** Số lá Discard All mỗi màu. */
const DISCARD_ALL_PER_COLOR = 1;

/** Bộ 108 lá chuẩn + lá Discard All. */
export function createDeck() {
  const deck = [];
  for (const color of COLORS) {
    deck.push(makeCard(color, '0'));
    for (let n = 1; n <= 9; n++) {
      deck.push(makeCard(color, String(n)), makeCard(color, String(n)));
    }
    for (const value of ['skip', 'reverse', 'draw2']) {
      deck.push(makeCard(color, value), makeCard(color, value));
    }
    for (let i = 0; i < DISCARD_ALL_PER_COLOR; i++) deck.push(makeCard(color, 'discard_all'));
  }
  for (let i = 0; i < 4; i++) {
    deck.push(makeCard('wild', 'wild'), makeCard('wild', 'wild4'));
  }
  return deck;
}

export function shuffle(list) {
  const a = [...list];
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}

const ORDER = { red: 0, yellow: 1, green: 2, blue: 3, wild: 4 };
const VALUE_ORDER = ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'skip', 'reverse', 'draw2', 'discard_all', 'wild', 'wild4'];

/** Sắp xếp bài trên tay theo màu rồi theo giá trị. */
export function sortHand(cards) {
  return [...cards].sort(
    (a, b) => ORDER[a.color] - ORDER[b.color] || VALUE_ORDER.indexOf(a.value) - VALUE_ORDER.indexOf(b.value),
  );
}
