import { Navigate, Route, Routes } from 'react-router-dom';
import Stage from './components/Stage.jsx';
import Login from './pages/Login.jsx';
import Lobby from './pages/Lobby.jsx';
import CustomRoom from './pages/CustomRoom.jsx';
import Matchmaking from './pages/Matchmaking.jsx';
import GameTable from './pages/GameTable.jsx';
import TeamRoom from './pages/TeamRoom.jsx';
import MatchResult from './pages/MatchResult.jsx';

export default function App() {
  return (
    <Stage>
      <Routes>
        <Route path="/" element={<Login />} />
        <Route path="/lobby" element={<Lobby />} />
        <Route path="/custom" element={<CustomRoom />} />
        <Route path="/matchmaking/:mode" element={<Matchmaking />} />
        <Route path="/room/:mode" element={<TeamRoom />} />
        <Route path="/game/:mode" element={<GameTable />} />
        <Route path="/result" element={<MatchResult />} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    </Stage>
  );
}
