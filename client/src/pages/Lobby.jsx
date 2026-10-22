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
import '../components/lobby/lobby.css';

const START_INDEX = Math.max(0, LOBBY_MODES.findIndex((m) => m.key === DEFAULT_MODE));

export default function Lobby() {
  const navigate = useNavigate();
  const { me } = useSocial();
  const [active, setActive] = useState(START_INDEX);
  const [modal, setModal] = useState(null); // 'rank' | 'rules' | 'profile' | 'settings' | null
  const { settings, set: setSetting, reset: resetSettings } = useSettings();
  const [toast, setToast] = useState(null);

  const notify = (text) => setToast({ id: Date.now(), text });
  const closeModal = () => setModal(null);
  const enter = (m) => navigate(m.to);
  const mode = LOBBY_MODES[active];

  return (
    <div className="screen lobby-screen">
      <MenuBackground />

      <TopBar
        me={me}
        muted={settings.muted}
        onToggleMute={() => setSetting('muted', !settings.muted)}
        onProfile={() => setModal('profile')}
        onSettings={() => setModal('settings')}
      />

      <div className="lobby__heading"><h1>Chọn chế độ chơi</h1><p>Một bộ bài, nhiều cách vui.</p></div>

      <main className="lobby__stage" aria-label="Bộ bài chế độ chơi">
        <ModeCarousel modes={LOBBY_MODES} active={active} onChange={setActive} onSelect={enter} keyboard={!modal} />
      </main>

      <section key={mode.key} className="lobby__info">
        <div className="lobby__selection">
        <h2 className="display lobby__mode-title">{mode.title}</h2>
        <span className={`chip lobby__chip ${mode.ranked ? 'is-ranked' : ''}`}>{mode.ranked ? 'XẾP HẠNG' : 'PHÒNG RIÊNG'}</span>
        </div>
        <p className="lobby__mode-desc">{mode.desc}</p>
        <div className="lobby__cta">
          <button type="button" className="btn btn--primary lobby__play" onClick={() => enter(mode)}>
            <IconPlay /> {mode.key === 'custom' ? 'TẠO PHÒNG RIÊNG' : 'CHƠI NGAY'}
          </button>
        </div>
      </section>

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

      <Dock
        items={[
          { key: 'friends', label: 'Bạn bè', icon: IconUsers, onClick: () => setModal('friends') },
          { key: 'rank', label: 'Bảng xếp hạng', icon: IconTrophy, onClick: () => setModal('rank') },
          { key: 'rules', label: 'Luật chơi', icon: IconBook, onClick: () => setModal('rules') },
        ]}
      />
      <div className="lobby__hint">← → chọn chế độ · Enter để chơi</div>

      <div className="lobby-friends-drawer"><FriendsModal
        open={modal === 'friends'}
        onClose={closeModal}
        notify={notify}
        onInvite={(f) => navigate('/custom', { state: { inviteFriendId: f.id } })}
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
        onLogout={() => navigate('/')}
      />

      {toast && <div key={toast.id} className="menu-toast">{toast.text}</div>}
    </div>
  );
}
