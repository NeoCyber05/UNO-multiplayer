import { useState } from 'react';
import UnoCard from '../components/card/UnoCard.jsx';
import { IconArrowRight, IconCards, IconTrophy, IconUsers } from '../components/Icons.jsx';
import { LOBBY_MODES } from '../data/lobbyModes';
import { ASSETS } from '../config/assets';
import { AVATARS } from '../data/avatars';
import { useSession } from '../net/session';
import { INVALID_DISPLAY_NAME, validDisplayName } from '../net/displayName';
import './pages.css';

const HERO_CARDS = [
  { card: { id: 'h1', color: 'blue', value: '5' }, x: 20, y: 40, r: -20 },
  { card: { id: 'h2', color: 'green', value: 'reverse' }, x: 104, y: 12, r: -7 },
  { card: { id: 'h3', color: 'red', value: 'draw2' }, x: 188, y: 14, r: 7 },
  { card: { id: 'h4', color: 'yellow', value: '7' }, x: 272, y: 40, r: 20 },
];

export default function Login() {
  const { request, connection } = useSession();
  const [email, setEmail] = useState('');
  const [pass, setPass] = useState('');
  const [error, setError] = useState('');
  const [registering, setRegistering] = useState(false);
  const [name, setName] = useState('');
  const [busy, setBusy] = useState(false);

  const submit = async (e) => {
    e.preventDefault();
    if (busy || connection !== 'connected') return;
    if (!/^\S+@\S+\.\S+$/.test(email)) return setError('Email không hợp lệ');
    if (pass.length < 6) return setError('Mật khẩu tối thiểu 6 ký tự');
    if (registering && !validDisplayName(name)) return setError(INVALID_DISPLAY_NAME);
    setError('');
    setBusy(true);
    const credentials = { email: email.trim().toLowerCase(), password: pass };
    try {
      if (registering) {
        await request('REGISTER', { ...credentials, name: name.trim(), avatar: AVATARS[0].key });
        setRegistering(false);
      }
      await request('LOGIN', credentials);
    } catch (error) {
      setError(error.message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="screen login">
      <aside
        className="login__hero"
        style={ASSETS.backgrounds.login ? { backgroundImage: `url(${ASSETS.backgrounds.login})` } : undefined}
      >
        <span className="login__edition eyebrow">UNO ONLINE · CÙNG NHAU CHƠI</span>
        <div className="login__hero-glow" />
        <div className="login__cards">
          {HERO_CARDS.map((h, i) => (
            <div
              key={h.card.id}
              className="login__card"
              style={{ left: h.x, top: h.y, '--r': `${h.r}deg`, animationDelay: `${i * 0.12}s` }}
            >
              <UnoCard card={h.card} width={104} />
            </div>
          ))}
        </div>

        {ASSETS.logo ? (
          <img className="login__logo-img" src={ASSETS.logo} alt="UNO" />
        ) : (
          <div className="login__logo display">UNO</div>
        )}
        <div className="login__tagline">Bộ bài bạn bè, mọi lúc mọi nơi</div>

        <div className="login__stats">
          <div><IconUsers /><b className="display">4</b><span>Người mỗi bàn</span></div>
          <div><IconCards /><b className="display">{LOBBY_MODES.length}</b><span>Chế độ chơi</span></div>
          <div><IconTrophy /><b className="display">ELO</b><span>Thử sức xếp hạng</span></div>
        </div>
      </aside>

      <main className="login__main">
        <form className="login__form" onSubmit={submit} noValidate>
          <div>
            <span className="eyebrow">SẴN SÀNG CHO VÁN TIẾP THEO?</span>
            <h1 className="display login__title">{registering ? 'Tạo tài khoản' : 'Chào mừng trở lại.'}</h1>
            <div className="muted" style={{ fontSize: 14, marginTop: 4 }}>Vào sảnh chờ và tìm bạn chơi cùng</div>
          </div>

          {registering && <div>
            <label className="label" htmlFor="name">Tên hiển thị</label>
            <input id="name" className="input" value={name}
              onChange={(e) => setName(e.target.value)} autoComplete="nickname" />
          </div>}
          <div>
            <label className="label" htmlFor="email">Email</label>
            <input
              id="email"
              className={`input ${error && error.startsWith('Email') ? 'is-error' : ''}`}
              type="email"
              placeholder="ban@vidu.com"
              value={email}
              onChange={(e) => setEmail(e.target.value)}
              autoComplete="email"
            />
          </div>
          <div>
            <label className="label" htmlFor="pass">Mật khẩu</label>
            <input
              id="pass"
              className={`input ${error && error.startsWith('Mật') ? 'is-error' : ''}`}
              type="password"
              placeholder="••••••••"
              value={pass}
              onChange={(e) => setPass(e.target.value)}
              autoComplete={registering ? 'new-password' : 'current-password'}
            />
          </div>

          <div className="login__row">
            <span className="login__error" role="alert">{error}</span>
            {connection !== 'connected' && connection !== 'reconnecting' && <span role="status">
              {connection === 'unavailable' ? 'Mở UNO Desktop để kết nối.' :
                connection === 'connecting' ? 'Đang kết nối…' : 'Máy chủ chưa kết nối.'}
            </span>}
          </div>

          <button type="submit" className="btn btn--primary btn--block" disabled={busy || connection !== 'connected'}>
            {registering ? 'Đăng ký' : 'Vào sảnh chơi'} <IconArrowRight />
          </button>

          <div className="muted" style={{ textAlign: 'center', fontSize: 13 }}>
            <button type="button" className="link-btn link-btn--yellow" disabled={busy}
              onClick={() => { setRegistering(!registering); setError(''); }}>
              {registering ? 'Đã có tài khoản? Đăng nhập' : 'Chưa có tài khoản? Đăng ký'}
            </button>
          </div>
        </form>
      </main>
    </div>
  );
}
