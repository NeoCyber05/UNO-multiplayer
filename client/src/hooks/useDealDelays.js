import { useRef } from 'react';

/**
 * Độ trễ animation (giây) cho từng lá, gán một lần lúc lá mới xuất hiện:
 * nhiều lá vào cùng lúc (chia bài, bị +2/+4) bay vào lần lượt.
 * Giữ cố định theo id vì đổi animation-delay giữa chừng sẽ làm giật.
 */
export default function useDealDelays(cards, stagger) {
  const delays = useRef(new Map());
  const ids = new Set(cards.map((c) => c.id));
  for (const id of delays.current.keys()) if (!ids.has(id)) delays.current.delete(id);
  let k = 0;
  for (const c of cards) if (!delays.current.has(c.id)) delays.current.set(c.id, k++ * stagger);
  return delays.current;
}
