let ctx;

/** Âm "ting" ngắn báo tới lượt. volume 0-100. */
export default function playTurnChime(volume = 80) {
  try {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    ctx = ctx || new AC();
    if (ctx.state === 'suspended') ctx.resume();
    const t = ctx.currentTime;
    const gain = ctx.createGain();
    gain.connect(ctx.destination);
    gain.gain.setValueAtTime(0.0001, t);
    gain.gain.exponentialRampToValueAtTime(Math.max(0.0001, (volume / 100) * 0.25), t + 0.02);
    gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.45);
    [880, 1318.5].forEach((f, i) => {
      const o = ctx.createOscillator();
      o.type = 'sine';
      o.frequency.value = f;
      o.connect(gain);
      o.start(t + i * 0.09);
      o.stop(t + 0.45);
    });
  } catch {
    // trình duyệt chặn audio trước khi người dùng tương tác: bỏ qua
  }
}
