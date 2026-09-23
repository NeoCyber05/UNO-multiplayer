import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import UnoCard from '../components/card/UnoCard.jsx';
import { ASSETS } from '../config/assets';
import './pages.css';

const HERO_CARDS = [
  { card: { id: 'h1', color: 'blue', value: '5' }, x: 20, y: 40, r: -20 },
  { card: { id: 'h2', color: 'green', value: 'reverse' }, x: 104, y: 12, r: -7 },
  { card: { id: 'h3', color: 'red', value: 'draw2' }, x: 188, y: 14, r: 7 },
  { card: { id: 'h4', color: 'yellow', value: '7' }, x: 272, y: 40, r: 20 },
];

export default function Login() {
  const navigate = useNavigate();
  const [email, setEmail] = useState('');
  const [pass, setPass] = useState('');
  const [error, setError] = useState('');

  const submit = (e) => {
    e.preventDefault();
    if (!/^\S+@\S+\.\S+$/.test(email)) return setError('Email không hợp lệ');
    if (pass.length < 6) return setError('Mật khẩu tối thiểu 6 ký tự');
    setError('');
    navigate('/lobby');
  };

  return (
    <div className="screen login">
      <aside
        className="login__hero"
        style={ASSETS.backgrounds.login ? { backgroundImage: `url(${ASSETS.backgrounds.login})` } : undefined}
      >
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
          <div><b className="display" style={{ color: 'var(--yellow)' }}>120K+</b><span>Người chơi</span></div>
          <div><b className="display" style={{ color: 'var(--green)' }}>3</b><span>Chế độ chơi</span></div>
          <div><b className="display" style={{ color: 'var(--blue)' }}>24/7</b><span>Luôn có trận</span></div>
        </div>
      </aside>

      <main className="login__main">
        <form className="login__form" onSubmit={submit} noValidate>
          <div>
            <div className="display login__title">Đăng nhập</div>
            <div className="muted" style={{ fontSize: 14, marginTop: 4 }}>Vào sảnh chờ và tìm bạn chơi cùng</div>
          </div>

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
              autoComplete="current-password"
            />
          </div>

          <div className="login__row">
            <span className="login__error" role="alert">{error}</span>
            <button type="button" className="link-btn">Quên mật khẩu?</button>
          </div>

          <button type="submit" className="btn btn--primary btn--block">Đăng nhập</button>

          <div className="divider">hoặc</div>

          <button type="button" className="btn btn--ghost btn--block" onClick={() => navigate('/lobby')}>
            <span className="google-g">G</span>
            Đăng nhập bằng Google
          </button>
          <button type="button" className="btn btn--outline btn--block" onClick={() => navigate('/lobby')}>
            Chơi với tư cách khách
          </button>

          <div className="muted" style={{ textAlign: 'center', fontSize: 13 }}>
            Chưa có tài khoản? <button type="button" className="link-btn link-btn--yellow">Đăng ký miễn phí</button>
          </div>
        </form>
      </main>
    </div>
  );
}
