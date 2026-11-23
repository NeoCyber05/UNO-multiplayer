import { useEffect, useRef, useState } from 'react';
import Modal from '../Modal.jsx';
import UnoCard from '../card/UnoCard.jsx';
import { ASSETS } from '../../config/assets';
import './draft.css';

export function DraftTray({ player, active }) {
  return (
    <div className={`draft-tray ${active ? 'is-active' : ''}`} aria-label={`Bài đã chọn của ${player.name}`}>
      {Array.from({ length: 4 }, (_, round) => (
        <div className="draft-slot" key={round}>
          {player.hand[round + 4]
            ? <UnoCard card={player.hand[round + 4]} faceDown={!player.handVisible} width={56} />
            : <span>·</span>}
        </div>
      ))}
    </div>
  );
}

export default function DraftSelection({ state, onPick, hidden, canAct }) {
  const [closed, setClosed] = useState(false);
  const [now, setNow] = useState(performance.now());
  const flight = useRef(null);
  const draft = state.draft;
  const seat = state.turn;
  const round = Math.floor(draft.turn / 4);
  const open = !closed && !hidden;
  const seconds = Math.max(0, Math.min(20, Math.ceil((state.localDeadline - now) / 1000)));

  const pick = (index) => {
    if (!canAct || seat !== 0 || !open || !draft.offers[index]) return;
    onPick(index);
    if (flight.current) return;
    const source = document.querySelector(`[data-draft-offer="${index}"] .uno-card`);
    const target = document.querySelector('.hand');
    if (!source || !target) return;
    const from = source.getBoundingClientRect();
    const to = target.getBoundingClientRect();
    const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    const duration = reduced ? 140 : 420;
    const clone = document.createElement('img');
    clone.src = ASSETS.cardFace(draft.offers[index]);
    clone.className = 'draft-flight';
    clone.alt = '';
    clone.setAttribute('aria-hidden', 'true');
    Object.assign(clone.style, { left: `${from.x}px`, top: `${from.y}px`, width: `${from.width}px`, height: `${from.height}px` });
    document.body.append(clone);
    const width = 116 * from.width / 92;
    const x = to.x + to.width / 2 - width / 2;
    clone.animate(reduced ? [{ opacity: 1 }, { opacity: 0 }] : [
      { transform: 'translate(0, 0) scale(1)' },
      { transform: `translate(${x - from.x}px, ${to.y - from.y}px) scale(${width / from.width})` },
    ], { duration, easing: 'cubic-bezier(.16,1,.3,1)', fill: 'forwards' });
    const timer = setTimeout(() => {
      clone.remove();
      flight.current = null;
    }, duration);
    flight.current = { clone, timer };
  };

  useEffect(() => {
    setNow(performance.now());
    if (state.paused) return;
    const timer = setInterval(() => setNow(performance.now()), 200);
    return () => clearInterval(timer);
  }, [state.localDeadline, state.paused]);
  useEffect(() => () => {
    if (flight.current) {
      clearTimeout(flight.current.timer);
      flight.current.clone.remove();
    }
  }, []);
  return (
    <>
      <div className={`draft-scrim ${open ? 'is-open' : ''}`} />
      {closed && !hidden && <button className="btn btn--primary draft-resume" onClick={() => setClosed(false)}>Tiếp tục chọn bài</button>}
      <div className={`draft-overlay draft-overlay--${['bottom', 'left', 'top', 'right'][seat]}`}>
        <Modal open={open} onClose={() => setClosed(true)} className="draft-box" label="Chọn bài đầu ván">
          <div className="draft-box__head">
            <div><small>UNO DRAFT</small><h2 className="display">Vòng {round + 1} <i>/ 4</i></h2></div>
            <ol className="draft-rounds" aria-hidden="true">{[0, 1, 2, 3].map(r => <li key={r} className={r < round ? 'is-done' : r === round ? 'is-now' : ''} />)}</ol>
            <span className={`draft-timer ${seconds <= 3 ? 'is-low' : ''}`}>{seconds}s</span>
          </div>
          <div className="draft-offers" key={round}>
            {draft.offers.map((card, i) => (
              <button key={i} type="button" data-draft-offer={i} className={`draft-offer ${card ? '' : 'is-empty'}`}
                disabled={!card || seat !== 0 || !canAct} onClick={() => pick(i)}
                aria-label={card ? `Chọn ${card.color} ${card.value}` : 'Lá đã được chọn'}>
                {card ? <UnoCard card={card} width={92} /> : <span>Đã chọn</span>}
              </button>
            ))}
          </div>
          <h3 aria-live="polite">{seat === 0 ? 'Đến lượt bạn chọn bài' : `${state.players[seat].name} đang chọn bài`}</h3>
          <p>Mỗi người chọn 1 lá. Lá đã chọn sẽ được bỏ khỏi bộ bài chung.</p>
          <ol className="draft-order" aria-label="Thứ tự chọn trong vòng">
            {Array.from({ length: 4 }, (_, i) => (round + i - state.viewerSeat + 4) % 4).map((s, i) => <li key={s} aria-current={s === seat ? 'step' : undefined}><b>{String.fromCharCode(65 + state.players[s].seat)}</b> {s === 0 ? 'Bạn' : state.players[s].name}{i < 3 && <span>→</span>}</li>)}
          </ol>
          <footer>
            <div className="draft-progress"><span>{draft.turn} / 16 lượt chọn · mỗi vòng đổi người chọn đầu</span><i style={{ '--p': draft.turn / 16 }} /></div>
          </footer>
        </Modal>
      </div>
    </>
  );
}
