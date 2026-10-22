import { useEffect, useRef, useState } from 'react';
import Modal from '../Modal.jsx';
import UnoCard from '../card/UnoCard.jsx';
import { draftSeat } from '../../game/draft';
import { ASSETS } from '../../config/assets';
import './draft.css';

export function DraftTray({ player, seat, active }) {
  return (
    <div className={`draft-tray ${active ? 'is-active' : ''}`} aria-label={`Bài đã chọn của ${player.name}`}>
      {Array.from({ length: 4 }, (_, round) => (
        <div className="draft-slot" data-draft-target={`${seat}-${round}`} key={round}>
          {player.hand[round + 4] ? <UnoCard card={player.hand[round + 4]} width={56} /> : <span>·</span>}
        </div>
      ))}
    </div>
  );
}

export default function DraftSelection({ state, onPick, paused }) {
  const [ready, setReady] = useState(false);
  const [closed, setClosed] = useState(false);
  const [flying, setFlying] = useState(null);
  const [seconds, setSeconds] = useState(10);
  const flight = useRef(null);
  const draft = state.draft;
  const seat = draftSeat(draft);
  const round = Math.floor(draft.turn / 4);
  const remaining = draft.offers.map((card, i) => card ? i : null).filter(i => i != null);
  const forced = remaining.length === 1;
  const open = ready && !closed && !paused;
  const latest = useRef(null);

  const pick = (index) => {
    if (flight.current || !open || !draft.offers[index]) return;
    const source = document.querySelector(`[data-draft-offer="${index}"] .uno-card`);
    const target = document.querySelector(seat === 0 ? '.hand' : `[data-draft-target="${seat}-${round}"]`);
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
    const width = seat === 0 ? 116 * from.width / 120 : to.width;
    const x = seat === 0 ? to.x + to.width / 2 - width / 2 : to.x;
    clone.animate(reduced ? [{ opacity: 1 }, { opacity: 0 }] : [
      { transform: 'translate(0, 0) scale(1)' },
      { transform: `translate(${x - from.x}px, ${to.y - from.y}px) scale(${width / from.width})` },
    ], { duration, easing: 'cubic-bezier(.16,1,.3,1)', fill: 'forwards' });
    const timer = setTimeout(() => {
      clone.remove();
      flight.current = null;
      setFlying(null);
      onPick(index, draft.turn, seat);
    }, duration);
    flight.current = { clone, timer };
    setFlying(index);
  };
  latest.current = pick;

  useEffect(() => {
    const timer = setTimeout(() => setReady(true), 850);
    return () => clearTimeout(timer);
  }, []);
  useEffect(() => () => {
    if (flight.current) {
      clearTimeout(flight.current.timer);
      flight.current.clone.remove();
    }
  }, []);
  useEffect(() => {
    if (!open || flying != null) return undefined;
    setSeconds(10);
    const delay = forced ? 600 : seat === 0 ? 10000 : 1300;
    const timer = setTimeout(() => latest.current(remaining[Math.floor(Math.random() * remaining.length)]), delay);
    const tick = setInterval(() => setSeconds(n => Math.max(0, n - 1)), 1000);
    return () => { clearTimeout(timer); clearInterval(tick); };
    // The offers and owner change together with the stamped turn.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [draft.turn, open, flying]);

  return (
    <>
      <div className={`draft-scrim ${open ? 'is-open' : ''}`} />
      <div className="draft-heading"><b className="display">UNO DRAFT</b><span>Chọn bài đầu ván · 4 vòng</span></div>
      {ready && closed && !paused && <button className="btn btn--primary draft-resume" onClick={() => setClosed(false)}>Tiếp tục chọn bài</button>}
      <div className={`draft-overlay draft-overlay--${['bottom', 'left', 'top', 'right'][seat]}`}>
        <Modal open={open} onClose={() => setClosed(true)} className="draft-box" label="Chọn bài đầu ván">
          <div className="draft-box__head"><small>UNO DRAFT</small><h2 className="display">Vòng {round + 1} / 4</h2><span>{forced ? 'AUTO' : seat === 0 ? `${seconds}s` : '···'}</span></div>
          <div className="draft-offers" key={round}>
            {draft.offers.map((card, i) => (
              <button key={i} type="button" data-draft-offer={i} className={`draft-offer ${card ? '' : 'is-empty'} ${flying === i ? 'is-departing' : ''}`}
                disabled={!card || seat !== 0 || forced || flying != null} onClick={() => pick(i)}
                aria-label={card ? `Chọn ${card.color} ${card.value}` : 'Lá đã được chọn'}>
                {card ? <UnoCard card={card} width={120} /> : <span>Đã chọn</span>}
              </button>
            ))}
          </div>
          <h3 aria-live="polite">{forced ? `${seat === 0 ? 'Bạn' : state.players[seat].name} nhận lá còn lại` : seat === 0 ? 'Đến lượt bạn chọn bài' : `${state.players[seat].name} đang chọn bài`}</h3>
          <p>Mỗi người chọn 1 lá. Lá đã chọn sẽ được bỏ khỏi bộ bài chung.</p>
          <ol className="draft-order" aria-label="Thứ tự chọn trong vòng">
            {Array.from({ length: 4 }, (_, i) => (round + i) % 4).map((s, i) => <li key={s} aria-current={s === seat ? 'step' : undefined}><b>{String.fromCharCode(65 + s)}</b> {s === 0 ? 'Bạn' : state.players[s].name}{i < 3 && <span>→</span>}</li>)}
          </ol>
          <footer><span>{draft.turn} / 16 lượt chọn</span><span>Mỗi vòng đổi người chọn đầu</span><button type="button" onClick={() => setClosed(true)}>Xem bàn ↗</button></footer>
        </Modal>
      </div>
    </>
  );
}
