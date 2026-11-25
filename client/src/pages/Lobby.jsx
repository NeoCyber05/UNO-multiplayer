import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import MenuBackground from '../components/MenuBackground.jsx';
import TopBar from '../components/lobby/TopBar.jsx';
import ModeCarousel from '../components/lobby/ModeCarousel.jsx';
import Dock from '../components/lobby/Dock.jsx';
import LeaderboardModal from '../components/lobby/LeaderboardModal.jsx';
import RulesModal from '../components/lobby/RulesModal.jsx';
import ProfileModal from '../components/lobby/ProfileModal.jsx';
import LobbySettingsModal from '../components/lobby/LobbySettingsModal.jsx';
import FriendsModal from '../components/lobby/FriendsModal.jsx';
import useSettings from '../hooks/useSettings';
import { IconBook, IconPlay, IconTrophy, IconUsers } from '../components/Icons.jsx';
import { DEFAULT_MODE, LOBBY_MODES } from '../data/lobbyModes';
import { useSocial } from '../context/SocialContext.jsx';
import { useSession } from '../net/session';
import '../components/lobby/lobby.css';

const START_INDEX = Math.max(0, LOBBY_MODES.findIndex((m) => m.key === DEFAULT_MODE));

export default function Lobby() {
  const navigate = useNavigate();
  const { me } = useSocial();
  const { request, receive, invites, room, connection } = useSession();
  const [active, setActive] = useState(START_INDEX);
  const [modal, setModal] = useState(null); // 'rank' | 'rules' | 'profile' | 'settings' | null
  const { settings, set: setSetting, reset: resetSettings } = useSettings();
  const [toast, setToast] = useState(null);

  const notify = (text) => setToast({ id: Date.now(), text });
  const closeModal = () => setModal(null);
  const enter = (m) => navigate(m.to);
  const mode = LOBBY_MODES[active];
  const command = async (operation) => {
    try {
      await operation();
    } catch (error) {
      notify(error.message);
    }
  };
  const openPanel = (panel) => {
    setModal(panel);
    const queries = {
      rank: ['LEADERBOARD_GET', 'LEADERBOARD_STATE'],
      friends: ['FRIEND_LIST', 'FRIEND_STATE'],
      profile: ['PROFILE_GET', 'PROFILE_STATE'],
    };
    const query = queries[panel];
    if (query) command(async () => {
      const payload = await request(query[0]);
      receive(query[1], payload.profile ?? payload);
    });
  };
  const invite = (friend) => command(async () => {
    if (!room) await request('ROOM_CREATE', { mode: 'classic' });
    await request('INVITE_SEND', { userId: friend.id, kind: 'room' });
    navigate('/custom');
    notify(`Đã gửi lời mời tới ${friend.name}`);
  });

  return (
    <div className="screen lobby-screen">
      <MenuBackground />

      <TopBar
        me={me}
        muted={settings.muted}
        onToggleMute={() => setSetting('muted', !settings.muted)}
        onProfile={() => openPanel('profile')}
        onSettings={() => setModal('settings')}
      />

      <h1 className="lobby__heading display">Chọn chế độ chơi</h1>

      <main className="lobby__stage" aria-label="Bộ bài chế độ chơi">
        <ModeCarousel modes={LOBBY_MODES} active={active} onChange={setActive} onSelect={enter}
          keyboard={!modal && connection === 'connected'} />
      </main>

      <div className="lobby__dots">
        {LOBBY_MODES.map((m, i) => (
          <button
            key={m.key}
            type="button"
            className={`lobby__dot ${i === active ? 'is-on' : ''}`}
            onClick={() => setActive(i)}
            aria-label={m.title}
            aria-pressed={i === active}
          />
        ))}
      </div>

      <section className="lobby__info" aria-label="Chế độ đã chọn">
        <div key={mode.key} className="lobby__details">
          <div className="lobby__selection">
            <h2 className="display lobby__mode-title">{mode.title}</h2>
            <span className="chip lobby__chip">{mode.chip}</span>
          </div>
          <p className="lobby__mode-desc">{mode.desc}</p>
        </div>
        <div className="lobby__cta">
          <button type="button" className="btn btn--primary lobby__play" aria-label={mode.key === 'custom' ? 'Tạo phòng riêng' : `Chơi ngay · ${mode.title}`} onClick={() => enter(mode)}>
            <IconPlay /> {mode.key === 'custom' ? 'TẠO PHÒNG RIÊNG' : 'CHƠI NGAY'}
          </button>
        </div>
      </section>

      <Dock
        items={[
          { key: 'friends', label: 'Bạn bè', icon: IconUsers, onClick: () => openPanel('friends') },
          { key: 'rank', label: 'Bảng xếp hạng', icon: IconTrophy, onClick: () => openPanel('rank') },
          { key: 'rules', label: 'Luật chơi', icon: IconBook, onClick: () => setModal('rules') },
        ]}
      />
      <div className="lobby__hint">← → chọn chế độ · Enter để chơi</div>

      <div className="lobby-friends-drawer"><FriendsModal
        open={modal === 'friends'}
        onClose={closeModal}
        notify={notify}
        onInvite={invite}
      /></div>
      <LeaderboardModal open={modal === 'rank'} onClose={closeModal} />
      <RulesModal open={modal === 'rules'} onClose={closeModal} />
      <ProfileModal open={modal === 'profile'} onClose={closeModal} me={me} />
      <LobbySettingsModal
        open={modal === 'settings'}
        onClose={closeModal}
        settings={settings}
        onChange={setSetting}
        onReset={() => {
          resetSettings();
          notify('Đã khôi phục cài đặt mặc định');
        }}
        onLogout={() => command(async () => { await request('LOGOUT'); navigate('/'); })}
      />

      {invites.length > 0 && <aside className="panel menu-toast" aria-label="Lời mời chơi">
        {invites.map((invite) => <div key={invite.inviteId}>
          <b>{invite.senderName}</b> {invite.kind === 'room' ? 'mời vào phòng' : 'mời tổ đội'}
          {[true, false].map((accept) => <button key={String(accept)} type="button"
            className="btn btn--sm btn--ghost" disabled={connection !== 'connected'}
            onClick={() => command(async () => {
              await request('INVITE_REPLY', { inviteId: invite.inviteId, accept });
              if (accept) navigate(invite.kind === 'room' ? '/custom' : `/room/${invite.mode ?? '2v2'}`);
            })}>{accept ? 'Chấp nhận' : 'Từ chối'}</button>)}
        </div>)}
      </aside>}
      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}
