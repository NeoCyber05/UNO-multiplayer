/** Phát một file âm thanh ngắn. volume 0-100. */
export default function playSfx(src, volume = 80) {
  const a = new Audio(src);
  a.volume = Math.min(1, Math.max(0, volume / 100));
  a.play().catch(() => {}); // trình duyệt chặn audio trước khi người dùng tương tác: bỏ qua
}
