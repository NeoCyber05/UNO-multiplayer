import { useEffect, useRef, useState } from 'react';
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
import DraftSelection, { DraftTray } from '../components/game/DraftSelection.jsx';
import { draftSeat } from '../game/draft';
import { MODES, SEATS } from '../game/constants';
import { useUnoGame } from '../game/useUnoGame';
import { buildMatchSummary } from '../game/summary';
import useSettings from '../hooks/useSettings';
import playTurnChime from '../hooks/playTurnChime';
import playUno from '../hooks/playUno';
import playAction from '../hooks/playAction';
import playSfx from '../hooks/playSfx';
import { ASSETS } from '../config/assets';
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

  const { settings, set: setSetting } = useSettings();

  const current = state.players[state.turn];
  const sfx = !settings.muted && settings.sfxOn;

  useEffect(() => {
    if (isMyTurn && !state.winner && settings.turnSound && sfx) playTurnChime(settings.sfxVolume);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [turnKey]);

  // Lá chức năng vừa lên chồng đánh -> phát âm hiệu ứng (bỏ qua lá lật đầu ván)
  const topCard = state.discard[state.discard.length - 1];
  const lastTopId = useRef(topCard?.id);
  useEffect(() => {
    if (!topCard || topCard.id === lastTopId.current) return;
    lastTopId.current = topCard.id;
    if (sfx) playAction(topCard.value, settings.sfxVolume);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [topCard?.id]);

  // Tổng số lá trên tay tăng -> có người rút bài (bỏ qua lúc draft chia bài)
  const handTotal = state.players.reduce((n, p) => n + p.hand.length, 0);
  const lastHandTotal = useRef(handTotal);
  useEffect(() => {
    const grew = handTotal > lastHandTotal.current;
    lastHandTotal.current = handTotal;
    if (grew && !state.draft && sfx) playSfx(ASSETS.sounds.drawCard, settings.sfxVolume);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [handTotal]);

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
    <div className={`table ${state.draft ? 'is-drafting' : ''}`}>
      <TableBackground />

      {/* HUD */}
      <MatchClock startedAt={state.startedAt} stopped={!!state.winner} />
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
              isTurn={state.draft ? draftSeat(state.draft) === idx : state.turn === idx && !state.winner}
              drafting={!!state.draft}
              turnKey={turnKey}
              tone={toneOf(p, pos)}
              relation={relationOf(p)}
              bubble={state.bubbles[p.id]}
              bubbleSide={pos === 'left' ? 'right' : 'left'}
              size={pos === 'top' ? 88 : 92}
              arrowSide={pos === 'top' ? 'left' : 'top'}
            />
            {state.draft ? <DraftTray player={p} seat={idx} active={draftSeat(state.draft) === idx} /> : <div className={`seat__hand seat__hand--${pos}`}>
              <div className="fan-wrap">
                <Fan
                  cards={p.hand}
                  faceUp={faceUp}
                  cardWidth={faceUp ? 66 : pos === 'top' ? 62 : 64}
                  step={faceUp ? 30 : 24}
                  maxLength={pos === 'top' ? 340 : 270}
                />
              </div>
            </div>}
          </div>
        );
      })}

      {/* Giữa bàn */}
      {!state.draft && <CenterTable
        discard={state.discard}
        currentColor={state.currentColor}
        direction={state.direction}
        deckCount={state.drawPile.length}
        canDraw={isMyTurn && !state.hasDrawn && !state.pendingWild}
        onDraw={actions.draw}
        turnSeat={state.winner ? null : SEATS[state.turn]}
      />}

      {!state.draft && state.message && (
        <div key={state.message.id} className="toast" role="status">
          {state.message.text}
        </div>
      )}

      <div className="hand-bar">
        {!state.draft && !state.winner && (
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
            isTurn={state.draft ? draftSeat(state.draft) === 0 : isMyTurn}
            drafting={!!state.draft}
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

      {!state.draft && <UnoButton
          active={canSayUno}
          onClick={() => {
            if (sfx) playUno(settings.sfxVolume);
            actions.sayUno();
          }}
        />}
      {state.draft && <DraftSelection key={state.discard[0].id} state={state} onPick={actions.draftPick} paused={settingsOpen} />}

      {/* Hộp thoại */}
      <ColorPicker
        open={state.pendingWild?.player === 0}
        onPick={actions.chooseColor}
        onCancel={actions.cancelWild}
      />
      <SettingsModal
        open={settingsOpen}
        onClose={() => setSettingsOpen(false)}
        onLeave={() => navigate('/lobby')}
        turnSound={settings.turnSound}
        onTurnSound={(v) => setSetting('turnSound', v)}
        music={settings.musicOn}
        sound={settings.sfxOn}
        onSound={(v) => setSetting('sfxOn', v)}
        onMusic={(v) => setSetting('musicOn', v)}
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
