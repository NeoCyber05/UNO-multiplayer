import { useCallback, useEffect, useState } from 'react';

const KEY = 'uno.settings';

export const DEFAULT_SETTINGS = {
  muted: false,
  sfxVolume: 80,
  musicVolume: 40,
  vibrate: true,
  autoSort: true,
  highlightPlayable: true,
  confirmWild: false,
  animSpeed: 'normal', // 'slow' | 'normal' | 'fast'
  language: 'vi',
  friendInvites: true,
  showOnline: true,
};

function load() {
  try {
    return { ...DEFAULT_SETTINGS, ...JSON.parse(localStorage.getItem(KEY) || '{}') };
  } catch {
    return DEFAULT_SETTINGS;
  }
}

/** Cài đặt người chơi, lưu localStorage. Sau này đồng bộ lên server theo tài khoản. */
export default function useSettings() {
  const [settings, setSettings] = useState(load);

  useEffect(() => {
    localStorage.setItem(KEY, JSON.stringify(settings));
  }, [settings]);

  const set = useCallback((key, value) => setSettings((s) => ({ ...s, [key]: value })), []);
  const reset = useCallback(() => setSettings(DEFAULT_SETTINGS), []);

  return { settings, set, reset };
}
