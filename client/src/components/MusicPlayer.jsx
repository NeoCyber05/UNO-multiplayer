import { useEffect, useState } from 'react';
import { useLocation } from 'react-router-dom';
import { ASSETS } from '../config/assets';
import { loadSettings } from '../hooks/useSettings';

// Một Audio duy nhất cho cả trang, gắn lên window để HMR / remount không đẻ thêm bản phát song song
const music = () => window.__unoMusic ?? (window.__unoMusic = new Audio());

/** Nhạc nền toàn app: nhạc trận ở /game, nhạc menu ở các màn còn lại. */
export default function MusicPlayer() {
  const { pathname } = useLocation();
  const [settings, setSettings] = useState(loadSettings);

  useEffect(() => {
    const reload = () => setSettings(loadSettings());
    window.addEventListener('uno-settings', reload);
    window.addEventListener('storage', reload); // đổi cài đặt ở tab khác
    return () => {
      window.removeEventListener('uno-settings', reload);
      window.removeEventListener('storage', reload);
    };
  }, []);

  const src = pathname.startsWith('/game') ? ASSETS.sounds.gameMusic : ASSETS.sounds.menuMusic;
  const volume = settings.muted || !settings.musicOn ? 0 : settings.musicVolume / 100;

  useEffect(() => {
    const a = music();
    a.loop = true;
    if (!a.src.endsWith(src)) a.src = src;
    a.volume = volume;
    if (volume === 0) {
      a.pause();
      return undefined;
    }
    const play = () => a.play().catch(() => {});
    play();
    // Trình duyệt chặn autoplay tới lần tương tác đầu tiên -> thử lại khi người dùng bấm
    window.addEventListener('pointerdown', play, { once: true });
    return () => window.removeEventListener('pointerdown', play);
  }, [src, volume]);

  useEffect(() => () => music().pause(), []);

  return null;
}
