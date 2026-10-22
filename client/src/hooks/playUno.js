/** Giọng "Uno!" (speechSynthesis) + hợp âm ngắn. volume 0-100. */
export default function playUno(volume = 80) {
  try {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (AC) {
      const ctx = new AC();
      const t = ctx.currentTime;
      const gain = ctx.createGain();
      gain.connect(ctx.destination);
      gain.gain.setValueAtTime(0.0001, t);
      gain.gain.exponentialRampToValueAtTime(Math.max(0.0001, (volume / 100) * 0.22), t + 0.02);
      gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.7);
      [523.25, 659.25, 783.99, 1046.5].forEach((f, i) => {
        const o = ctx.createOscillator();
        o.type = 'triangle';
        o.frequency.value = f;
        o.connect(gain);
        o.start(t + i * 0.06);
        o.stop(t + 0.7);
      });
      setTimeout(() => ctx.close(), 1000);
    }
    const u = new SpeechSynthesisUtterance('Uno!');
    u.lang = 'en-US';
    u.rate = 0.9;
    u.pitch = 1.3;
    u.volume = volume / 100;
    window.speechSynthesis.cancel();
    window.speechSynthesis.speak(u);
  } catch {
    // audio/speech bị chặn hoặc không hỗ trợ: bỏ qua
  }
}
