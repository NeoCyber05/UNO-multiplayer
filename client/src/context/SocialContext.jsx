import { createContext, useCallback, useContext, useMemo, useState } from 'react';
import { FRIENDS, ME } from '../data/mockData';

/**
 * Trạng thái "xã hội" dùng chung giữa các màn hình: hồ sơ của bạn (ELO trong phiên),
 * bạn bè / lời mời kết bạn, nhóm đang tổ đội và kết quả trận vừa chơi.
 * Sau này thay phần bên trong bằng dữ liệu từ server — các trang giữ nguyên.
 */
const SocialContext = createContext(null);

export function SocialProvider({ children }) {
  const [me, setMe] = useState(() => {
    try {
      return { ...ME, ...JSON.parse(localStorage.getItem('uno.profile') || '{}') };
    } catch {
      return ME;
    }
  });
  const [friends, setFriends] = useState(FRIENDS);
  const [party, setParty] = useState({ mode: null, teammate: null });
  const [lastMatch, setLastMatch] = useState(null);

  const friendState = useCallback(
    (id) => {
      const f = friends.find((x) => x.id === id);
      if (!f) return 'none';
      return f.pending ? 'pending' : 'friend';
    },
    [friends],
  );

  /** Gửi lời mời kết bạn: thêm vào danh sách ở trạng thái chờ. Bỏ qua nếu đã có. */
  const sendFriendRequest = useCallback((person) => {
    setFriends((list) =>
      list.some((f) => f.id === person.id)
        ? list
        : [
            ...list,
            {
              id: person.id,
              name: person.name,
              initials: person.initials,
              tint: person.tint,
              elo: person.elo,
              status: 'Đã gửi lời mời kết bạn',
              online: false,
              pending: true,
            },
          ],
    );
  }, []);

  const applyElo = useCallback((delta) => setMe((m) => ({ ...m, elo: m.elo + delta })), []);

  /** Đổi tên / avatar của mình; lưu localStorage (sau này gửi server). */
  const updateProfile = useCallback(({ name, avatar }) => {
    setMe((m) => {
      const n = (name ?? m.name).trim().slice(0, 16) || m.name;
      const next = { ...m, name: n, avatar: avatar ?? m.avatar, initials: n.slice(0, 2).toUpperCase() };
      try {
        localStorage.setItem('uno.profile', JSON.stringify({ name: next.name, avatar: next.avatar, initials: next.initials }));
      } catch { /* bỏ qua: không lưu được */ }
      return next;
    });
  }, []);

  const value = useMemo(
    () => ({ me, applyElo, updateProfile, friends, friendState, sendFriendRequest, party, setParty, lastMatch, setLastMatch }),
    [me, applyElo, updateProfile, friends, friendState, sendFriendRequest, party, lastMatch],
  );

  return <SocialContext.Provider value={value}>{children}</SocialContext.Provider>;
}

export function useSocial() {
  const ctx = useContext(SocialContext);
  if (!ctx) throw new Error('useSocial phải nằm trong <SocialProvider>');
  return ctx;
}
