import { useLayoutEffect, useRef } from 'react';
import { ASSETS } from '../../config/assets';

/** Dùng số lá thực nhận vì chồng rút có thể hết giữa lượt phạt. */
export default function DrawFlight({ players }) {
  const layer = useRef(null);
  const previous = useRef(players);

  useLayoutEffect(() => {
    const before = previous.current;
    previous.current = players;
    if (before === players || matchMedia('(prefers-reduced-motion: reduce)').matches) return;

    const overlay = layer.current;
    const table = overlay.parentElement;
    players.forEach((player, seat) => {
      const gained = player.hand.length - before[seat].hand.length;
      if (gained <= 0) return;

      const from = table.querySelector('.deck').getBoundingClientRect();
      const to = table.querySelector(`[data-draw-seat="${seat}"]`).getBoundingClientRect();
      const area = overlay.getBoundingClientRect();
      const scale = area.width / overlay.offsetWidth;
      const x = (to.left + to.width / 2 - from.left - from.width / 2) / scale;
      const y = (to.top + to.height / 2 - from.top - from.height / 2) / scale;

      for (let i = 0; i < gained; i++) {
        const card = document.createElement('img');
        card.className = 'draw-flight__card';
        card.alt = '';
        card.src = ASSETS.cardBack;
        card.style.left = `${(from.left + from.width / 2 - area.left) / scale}px`;
        card.style.top = `${(from.top + from.height / 2 - area.top) / scale}px`;
        overlay.append(card);
        const animation = card.animate([
          { transform: 'scale(.75)', opacity: 0 },
          { transform: `translate(${x * .38}px, ${y * .38 - 42}px) rotate(${Math.sign(x) * 14}deg)`, opacity: 1, offset: .38 },
          { opacity: 1, offset: .82 },
          { transform: `translate(${x}px, ${y}px) rotate(${Math.sign(x) * 26}deg) scale(.65)`, opacity: 0 },
        ], { duration: 460, delay: i * 90, easing: 'ease-in-out' });
        animation.onfinish = () => card.remove();
      }
    });
  }, [players]);

  return <div className="draw-flight" ref={layer} aria-hidden="true" />;
}
