import { useEffect, useRef } from 'react';
import { Navigate, Route, Routes, useLocation, useNavigate } from 'react-router-dom';
import Stage from './components/Stage.jsx';
import MusicPlayer from './components/MusicPlayer.jsx';
import Login from './pages/Login.jsx';
import Lobby from './pages/Lobby.jsx';
import CustomRoom from './pages/CustomRoom.jsx';
import Matchmaking from './pages/Matchmaking.jsx';
import GameTable from './pages/GameTable.jsx';
import TeamRoom from './pages/TeamRoom.jsx';
import MatchResult from './pages/MatchResult.jsx';
import { useSession } from './net/session';

export default function App() {
  const { me, game, connection } = useSession();
  const { pathname } = useLocation();
  const navigate = useNavigate();
  const userId = me?.userId;
  const reconnecting = connection === 'reconnecting';
  const matchId = game?.matchId;
  const mode = game?.mode;
  const routedMatch = useRef(null);
  useEffect(() => {
    if (!userId || !matchId) {
      routedMatch.current = null;
      return;
    }
    const routeKey = `${userId}:${matchId}`;
    if (routeKey !== routedMatch.current) {
      routedMatch.current = routeKey;
      navigate(`/game/${mode}`);
    }
  }, [userId, matchId, mode, navigate]);

  if (!me && pathname !== '/') return <Navigate to="/" replace />;

  return (
    <Stage>
      <MusicPlayer />
      {(reconnecting || (me && connection === 'disconnected')) && (
        <div className="connection-banner" role="status">
          {reconnecting ? 'Đang kết nối lại với máy chủ…' : 'Đã mất kết nối với máy chủ.'}
        </div>
      )}
      <div inert={reconnecting ? '' : undefined}>
        <Routes>
          <Route path="/" element={me ? <Navigate to={matchId ? `/game/${mode}` : '/lobby'} replace /> : <Login />} />
          <Route path="/lobby" element={<Lobby />} />
          <Route path="/custom" element={<CustomRoom />} />
          <Route path="/matchmaking/:mode" element={<Matchmaking />} />
          <Route path="/room/:mode" element={<TeamRoom />} />
          <Route path="/game/:mode" element={<GameTable />} />
          <Route path="/result" element={<MatchResult />} />
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </div>
    </Stage>
  );
}
