import { ASSETS } from '../config/assets';
import playSfx from './playSfx';

let ctx;

/** Âm hiệu ứng khi đánh lá chức năng (wild, wild4, draw2, reverse, skip). volume 0-100. */
export default function playAction(value, volume = 80) {
  if (value === 'skip') {
    playSfx(ASSETS.sounds.skip, volume);
    return;
  }
  if (!['wild', 'wild4', 'draw2', 'reverse'].includes(value)) {
    playSfx(ASSETS.sounds.playCard, volume); // lá số thường
    return;
  }
  try {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    ctx = ctx || new AC();
    if (ctx.state === 'suspended') ctx.resume();
    const t = ctx.currentTime;
    const peak = Math.max(0.0001, (volume / 100) * 0.22);

    // Một nốt: tần số trượt f1 -> f2 trong dur giây
    const tone = (start, f1, f2, dur, type = 'sine', gainMul = 1) => {
      const g = ctx.createGain();
      g.connect(ctx.destination);
      g.gain.setValueAtTime(0.0001, t + start);
      g.gain.exponentialRampToValueAtTime(peak * gainMul, t + start + 0.015);
      g.gain.exponentialRampToValueAtTime(0.0001, t + start + dur);
      const o = ctx.createOscillator();
      o.type = type;
      o.frequency.setValueAtTime(f1, t + start);
      o.frequency.exponentialRampToValueAtTime(f2, t + start + dur);
      o.connect(g);
      o.start(t + start);
      o.stop(t + start + dur + 0.02);
    };

    if (value === 'reverse') {
      // "vút - vút": quét lên rồi quét xuống
      tone(0, 260, 1100, 0.22, 'triangle');
      tone(0.2, 1100, 260, 0.26, 'triangle');
    } else if (value === 'draw2') {
      tone(0, 520, 260, 0.16, 'square', 0.6);
      tone(0.15, 520, 260, 0.2, 'square', 0.6);
    } else if (value === 'wild4') {
      [0, 0.1, 0.2, 0.3].forEach((s, i) => tone(s, 600 - i * 70, 300 - i * 40, 0.14, 'sawtooth', 0.45));
      tone(0.38, 160, 50, 0.45, 'sine', 1.4);
    } else if (value === 'wild') {
      // arpeggio lấp lánh đi lên
      [523.25, 659.25, 783.99, 1046.5, 1318.5].forEach((f, i) => tone(i * 0.06, f, f * 1.01, 0.3, 'sine', 0.8));
    }
  } catch {
    // trình duyệt chặn audio trước khi người dùng tương tác: bỏ qua
  }
}
