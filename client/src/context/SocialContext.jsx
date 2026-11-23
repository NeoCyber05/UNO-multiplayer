import { useSession } from '../net/session';
import { INVALID_DISPLAY_NAME, validDisplayName } from '../net/displayName';

const presence = {
  idle: 'Rảnh', online: 'Đang ở sảnh chờ', in_match: 'Đang trong trận', offline: 'Offline',
};

export function asPerson(player) {
  return {
    ...player, id: player.userId ?? player.id,
    initials: player.name?.slice(0, 2).toUpperCase(),
  };
}

export function useSocial() {
  const session = useSession();
  const me = session.me && asPerson(session.me);
  const friends = session.friends.map((friend) => {
    const status = friend.presence ?? friend.status ?? 'offline';
    const online = session.connection === 'connected' && (friend.online ?? status !== 'offline');
    return { ...asPerson(friend), online, status: online ? presence[status] ?? status : 'Offline' };
  });
  const friendState = (id) => {
    const friend = friends.find((entry) => entry.id === id);
    return friend ? (friend.pending ? 'pending' : 'friend') : 'none';
  };
  const sendFriendRequest = (person) => session.request('FRIEND_REQUEST', {
    tag: person.tag ?? `UNO#${person.id}`,
  });
  const updateProfile = async ({ name = me.name, avatar = me.avatar }) => {
    if (!validDisplayName(name)) throw new Error(INVALID_DISPLAY_NAME);
    const response = await session.request('PROFILE_UPDATE', { name: name.trim(), avatar });
    if (!response.profile) {
      const updated = await session.request('PROFILE_GET');
      if (updated.profile ?? updated.userId) session.receive('PROFILE_STATE', updated.profile ?? updated);
    }
  };
  return { me, friends, friendState, sendFriendRequest, updateProfile };
}
