import { useEffect, useState } from 'react';
import { useLocation, useNavigate, useParams } from 'react-router-dom';
import { IconGear, IconSmile } from '../components/Icons.jsx';
import TableBackground from '../components/game/TableBackground.jsx';
import PlayerBadge from '../components/game/PlayerBadge.jsx';
import Fan from '../components/game/Fan.jsx';
import CenterTable from '../components/game/CenterTable.jsx';
import MyHand from '../components/game/MyHand.jsx';
import UnoButton from '../components/game/UnoButton.jsx';
import ColorPicker from '../components/game/ColorPicker.jsx';
import QuickChat from '../components/game/QuickChat.jsx';
import MatchClock from '../components/game/MatchClock.jsx';
import SettingsModal from '../components/game/SettingsModal.jsx';
import { MODES, SEATS } from '../game/constants';
import { useUnoGame } from '../game/useUnoGame';
import { buildMatchSummary } from '../game/summary';
import { useSocial } from '../context/SocialContext.jsx';
import '../components/game/game.css';

// Hết ván -> chờ chút cho người chơi thấy lá cuối rồi sang trang tổng kết
const RESULT_DELAY_MS = 1500;

function GameScreen({ modeKey, lineup, ranked }) {
  const cfg = MODES[modeKey];
  const navigate = useNavigate();
  const { setLastMatch, applyElo } = useSocial();
  const { state, me, isMyTurn, playableIds, canSayUno, turnKey, actions } = useUnoGame(modeKey, lineup);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [chatOpen, setChatOpen] = useState(false);

  const current = state.players[state.turn];

  useEffect(() => {
    if (!state.winner) return undefined;
    const t = setTimeout(() => {
      const summary = buildMatchSummary(state, { ranked });
      setLastMatch(summary);
      if (summary.eloDelta != null) applyElo(summary.eloDelta);
      navigate('/result', { replace: true });
    }, RESULT_DELAY_MS);
    return () => clearTimeout(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.winner]);

  const relationOf = (p) => {
    if (!cfg.teams || p.isHuman) return null;
    return p.team === me.team ? { kind: 'team', label: 'Đồng đội' } : { kind: 'enemy', label: 'Đối thủ' };
  };
  const toneOf = (p, pos) => {
    if (cfg.teams) return p.team === me.team ? 'blue' : 'orange';
    return pos === 'top' ? 'orange' : 'blue';
  };

  return (
    <div className="table">
      <TableBackground />

      {/* HUD */}
      <MatchClock startedAt={state.startedAt} stopped={!!state.winner} />
      <div className="hud-mode">
        <span className="dot" style={{ background: cfg.accent }} />
        {cfg.label}
        <span className="hud-mode__sep" />
        Chồng bài: {state.drawPile.length}
      </div>
      <div className="hud-actions">
        <button type="button" className="hud-btn" onClick={() => setSettingsOpen(true)} aria-label="Cài đặt">
          <IconGear size={28} />
        </button>
      </div>

      {/* Người chơi khác */}
      {state.players.slice(1).map((p, k) => {
        const idx = k + 1;
        const pos = SEATS[idx];
        const faceUp = cfg.teammateOpen && p.team === me.team;
        return (
          <div key={p.id} className={`seat seat--${pos}`}>
            <PlayerBadge
              player={p}
              isTurn={state.turn === idx && !state.winner}
              turnKey={turnKey}
              tone={toneOf(p, pos)}
              relation={relationOf(p)}
              bubble={state.bubbles[p.id]}
              bubbleSide={pos === 'left' ? 'right' : 'left'}
              size={pos === 'top' ? 88 : 92}
              arrowSide={pos === 'top' ? 'left' : 'top'}
            />
            <div className={`seat__hand seat__hand--${pos}`}>
              <div className="fan-wrap">
                <Fan
                  cards={p.hand}
                  faceUp={faceUp}
                  cardWidth={faceUp ? 66 : pos === 'top' ? 62 : 64}
                  step={faceUp ? 30 : 24}
                  maxLength={pos === 'top' ? 340 : 270}
                />
              </div>
            </div>
          </div>
        );
      })}

      {/* Giữa bàn */}
      <CenterTable
        discard={state.discard}
        currentColor={state.currentColor}
        direction={state.direction}
        deckCount={state.drawPile.length}
        canDraw={isMyTurn && !state.hasDrawn && !state.pendingWild}
        onDraw={actions.draw}
        turnSeat={state.winner ? null : SEATS[state.turn]}
      />

      {state.message && (
        <div key={state.message.id} className="toast" role="status">
          {state.message.text}
        </div>
      )}

      <div className="hand-bar">
        {!state.winner && (
          <div className={`turn-pill ${isMyTurn ? 'is-mine' : ''}`}>
            <span className="turn-pill__dot" />
            {isMyTurn
              ? state.hasDrawn
                ? 'Đánh lá vừa rút hoặc bỏ lượt'
                : playableIds.size
                  ? 'Lượt của bạn'
                  : 'Không có lá phù hợp — hãy rút bài'
              : `Lượt của ${current.name}`}
          </div>
        )}
        {isMyTurn && state.hasDrawn && (
          <button type="button" className="btn btn--ghost btn--sm pass-btn" onClick={actions.pass}>
            Bỏ lượt
          </button>
        )}
      </div>

      <div className="hand-area">
        {/* Khu vực của bạn — avatar bám sát mép trái bài trên tay */}
        <div className="me">
          <div className="me__tools">
            <button type="button" className="emoji-btn" onClick={() => setChatOpen((v) => !v)} aria-label="Chat nhanh">
              <IconSmile size={30} />
              <span>chat</span>
            </button>
            {chatOpen && <QuickChat onSend={actions.chat} onClose={() => setChatOpen(false)} />}
          </div>
          <PlayerBadge
            player={me}
            isTurn={isMyTurn}
            turnKey={turnKey}
            tone="gold"
            bubble={state.bubbles[me.id]}
            bubbleSide="right"
            size={96}
          />
        </div>
        <MyHand
          cards={me.hand}
          playableIds={playableIds}
          isMyTurn={isMyTurn && !state.pendingWild}
          drawnCardId={state.drawnCardId}
          onPlay={actions.play}
        />
      </div>

      <UnoButton active={canSayUno} onClick={actions.sayUno} />

      {/* Hộp thoại */}
      <ColorPicker
        open={state.pendingWild?.player === 0}
        onPick={actions.chooseColor}
        onCancel={actions.cancelWild}
      />
      <SettingsModal
        open={settingsOpen}
        onClose={() => setSettingsOpen(false)}
        onRestart={() => {
          actions.restart();
          setSettingsOpen(false);
        }}
        onLeave={() => navigate('/lobby')}
      />
    </div>
  );
}

export default function GameTable() {
  const { mode } = useParams();
  const location = useLocation();
  const modeKey = MODES[mode] ? mode : 'classic';
  // Đội hình + ranked do Ghép trận / Phòng Custom truyền sang. Thiếu (F5) -> đội hình bot, không tính ELO.
  const lineup = location.state?.lineup ?? null;
  const ranked = !!lineup && !!location.state?.ranked;
  return <GameScreen key={location.key} modeKey={modeKey} lineup={lineup} ranked={ranked} />;
}
