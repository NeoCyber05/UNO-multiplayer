// Avatar dựng sẵn bằng SVG (viewBox 64x64). person.avatar = key trong danh sách này.
const eyes = (y = 30, dx = 9, c = '#243b53') => (
  <>
    <circle cx={32 - dx} cy={y} r="3" fill={c} />
    <circle cx={32 + dx} cy={y} r="3" fill={c} />
  </>
);
const smile = (y = 40) => <path d={`M25 ${y}q7 6 14 0`} fill="none" stroke="#243b53" strokeWidth="2.4" strokeLinecap="round" />;

// Mặt người: back = tóc sau đầu, front = tóc trước trán, extra = phụ kiện vẽ trên cùng.
const HAIR_SHORT = 'M16 31q-1-19 16-19t16 19q-3-8-9-10q-8 5-20 3q-3 3-3 7z';
const HAIR_BANGS = 'M17 29q1-16 15-16t15 16q-7-8-15-7q-7 1-15 7z';
const HAIR_LONG = 'M14 30q0-20 18-20t18 20v26h-36z';
const glasses = <g fill="none" stroke="#243b53" strokeWidth="2"><circle cx="26" cy="31" r="5" /><circle cx="38" cy="31" r="5" /><path d="M31 31h2" /></g>;
const human = ({ skin, hair, shirt, back, front, extra }) => (
  <>
    {back && <path d={back} fill={hair} />}
    <path d="M8 64q1-14 24-14t24 14z" fill={shirt} />
    <rect x="27" y="42" width="10" height="10" fill={skin} />
    <circle cx="17" cy="32" r="4" fill={skin} />
    <circle cx="47" cy="32" r="4" fill={skin} />
    <ellipse cx="32" cy="31" rx="15" ry="17" fill={skin} />
    {front && <path d={front} fill={hair} />}
    {eyes(31, 6, '#243b53')}
    <circle cx="21" cy="37" r="2.5" fill="#ff8da1" opacity=".5" />
    <circle cx="43" cy="37" r="2.5" fill="#ff8da1" opacity=".5" />
    {smile(39)}
    {extra}
  </>
);

export const AVATARS = [
  { key: 'boy', bg: '#9ec9ff', el: human({ skin: '#f6c9a0', hair: '#3b2a20', shirt: '#ff5262', front: HAIR_SHORT }) },
  { key: 'girl', bg: '#ffb3c1', el: human({ skin: '#f6d0b0', hair: '#5a3420', shirt: '#4c86ff', back: HAIR_LONG, front: HAIR_BANGS }) },
  { key: 'bun', bg: '#c5b3ff', el: human({ skin: '#e0a77a', hair: '#1f1a17', shirt: '#22c3d6', back: 'M32 2a8 8 0 110 16a8 8 0 110-16z', front: HAIR_BANGS }) },
  { key: 'afro', bg: '#ffd0a8', el: human({ skin: '#8d5a3b', hair: '#1f1a17', shirt: '#5cc47a', back: 'M32 4a22 20 0 110 40a22 20 0 110-40z', front: 'M18 26q2-12 14-12t14 12q-7-5-14-5t-14 5z' }) },
  { key: 'nerd', bg: '#a8e6ee', el: human({ skin: '#f2c29b', hair: '#b5651d', shirt: '#f2a93b', front: HAIR_SHORT, extra: glasses }) },
  { key: 'beard', bg: '#8fd3a4', el: human({ skin: '#d99a6c', hair: '#4a3020', shirt: '#2b3a67', front: HAIR_SHORT, extra: <path d="M17 33q0 19 15 19t15-19q-3 13-15 13t-15-13zM25 38q7-4 14 0q-7 2-14 0z" fill="#4a3020" /> }) },
  { key: 'cap', bg: '#fff0a8', el: human({ skin: '#f6c9a0', hair: '#3b2a20', shirt: '#ff5262', front: 'M16 31q-1-12 4-16v16z', extra: <path d="M15 25q1-15 17-15t17 15zM40 21h16q-1 5-8 5h-8z" fill="#4c86ff" /> }) },
  { key: 'punk', bg: '#2b3a67', el: human({ skin: '#f2c29b', hair: '#ff5ea8', shirt: '#243b53', front: 'M16 29l2-13 5 5 4-10 5 8 5-9 3 10 5-5 1 14q-8-7-15-7t-15 7z' }) },
  { key: 'grandpa', bg: '#b9c7ff', el: human({ skin: '#f0c6a4', hair: '#e6e9ee', shirt: '#8a5a3c', front: 'M16 33q-2-11 5-16v16zM48 33q2-11-5-16v16z', extra: <>{glasses}<path d="M24 41q4-3 8 0q4-3 8 0q-4 3-8 1q-4 2-8-1z" fill="#e6e9ee" /></> }) },
  { key: 'blonde', bg: '#7fb8ff', el: human({ skin: '#fbd7bd', hair: '#f5c542', shirt: '#ff9aa8', back: HAIR_LONG, front: HAIR_BANGS }) },
  { key: 'pigtails', bg: '#ffc94a', el: human({ skin: '#c98a5e', hair: '#2a1a12', shirt: '#c5b3ff', back: 'M8 30a7 7 0 1114 0a7 7 0 11-14 0zM42 30a7 7 0 1114 0a7 7 0 11-14 0z', front: HAIR_BANGS }) },
  { key: 'cat', bg: '#FFC94A', el: (<><path d="M12 28L14 8l14 11zM52 28L50 8 36 19z" fill="#f2a93b" /><circle cx="32" cy="36" r="20" fill="#f7b84a" />{eyes(34)}<path d="M29 40h6l-3 3z" fill="#e0556a" />{smile(43)}</>) },
  { key: 'bear', bg: '#8fd3a4', el: (<><circle cx="15" cy="17" r="8" fill="#a8744a" /><circle cx="49" cy="17" r="8" fill="#a8744a" /><circle cx="32" cy="35" r="21" fill="#c08b5c" /><ellipse cx="32" cy="42" rx="9" ry="7" fill="#ecd2b3" />{eyes(31)}<circle cx="32" cy="39" r="2.6" fill="#243b53" />{smile(44)}</>) },
  { key: 'fox', bg: '#9ec9ff', el: (<><path d="M10 30L12 6l16 14zM54 30L52 6 36 20z" fill="#e8742c" /><path d="M10 34q22 30 44 0L32 16z" fill="#f08a3c" /><path d="M20 40q12 14 24 0q-12-4-24 0z" fill="#fff" />{eyes(33, 10)}<circle cx="32" cy="45" r="2.4" fill="#243b53" /></>) },
  { key: 'panda', bg: '#c5b3ff', el: (<><circle cx="14" cy="16" r="8" fill="#243b53" /><circle cx="50" cy="16" r="8" fill="#243b53" /><circle cx="32" cy="35" r="21" fill="#fff" /><ellipse cx="22" cy="32" rx="5" ry="7" fill="#243b53" transform="rotate(-20 22 32)" /><ellipse cx="42" cy="32" rx="5" ry="7" fill="#243b53" transform="rotate(20 42 32)" /><circle cx="22" cy="32" r="1.8" fill="#fff" /><circle cx="42" cy="32" r="1.8" fill="#fff" /><circle cx="32" cy="42" r="2.6" fill="#243b53" /></>) },
  { key: 'frog', bg: '#ffd0a8', el: (<><circle cx="20" cy="18" r="9" fill="#5cc47a" /><circle cx="44" cy="18" r="9" fill="#5cc47a" /><ellipse cx="32" cy="38" rx="22" ry="18" fill="#6fd68a" /><circle cx="20" cy="18" r="4" fill="#fff" /><circle cx="44" cy="18" r="4" fill="#fff" /><circle cx="20" cy="18" r="2" fill="#243b53" /><circle cx="44" cy="18" r="2" fill="#243b53" /><path d="M18 42q14 10 28 0" fill="none" stroke="#243b53" strokeWidth="2.6" strokeLinecap="round" /></>) },
  { key: 'robot', bg: '#a8e6ee', el: (<><path d="M32 6v8" stroke="#5b7083" strokeWidth="3" /><circle cx="32" cy="6" r="3.5" fill="#ff5262" /><rect x="11" y="14" width="42" height="38" rx="10" fill="#cfd9e2" /><rect x="17" y="22" width="30" height="16" rx="8" fill="#243b53" /><circle cx="25" cy="30" r="3.5" fill="#22c3d6" /><circle cx="39" cy="30" r="3.5" fill="#22c3d6" /><path d="M24 45h16" stroke="#5b7083" strokeWidth="3" strokeLinecap="round" /></>) },
  { key: 'owl', bg: '#ffb3c1', el: (<><path d="M10 14l10 6M54 14l-10 6" stroke="#8a5a3c" strokeWidth="4" strokeLinecap="round" /><ellipse cx="32" cy="36" rx="22" ry="22" fill="#b07a52" /><circle cx="22" cy="31" r="9" fill="#fff" /><circle cx="42" cy="31" r="9" fill="#fff" /><circle cx="22" cy="31" r="4" fill="#243b53" /><circle cx="42" cy="31" r="4" fill="#243b53" /><path d="M28 40h8l-4 7z" fill="#f2a93b" /></>) },
  { key: 'bunny', bg: '#fff0a8', el: (<><ellipse cx="22" cy="14" rx="5" ry="13" fill="#f4f4f8" /><ellipse cx="42" cy="14" rx="5" ry="13" fill="#f4f4f8" /><ellipse cx="22" cy="15" rx="2.4" ry="9" fill="#ffb3c1" /><ellipse cx="42" cy="15" rx="2.4" ry="9" fill="#ffb3c1" /><circle cx="32" cy="38" r="19" fill="#f4f4f8" />{eyes(36)}<path d="M29 42h6l-3 3z" fill="#ff8da1" />{smile(45)}</>) },
  { key: 'ghost', bg: '#b9c7ff', el: (<><path d="M12 54V30a20 20 0 0140 0v24l-7-5-6 5-7-5-7 5-6-5z" fill="#fff" />{eyes(30, 8)}<ellipse cx="32" cy="40" rx="3.5" ry="4.5" fill="#243b53" /></>) },
  { key: 'alien', bg: '#2b3a67', el: (<><ellipse cx="32" cy="34" rx="20" ry="23" fill="#7be0a0" /><ellipse cx="23" cy="32" rx="6" ry="9" fill="#243b53" transform="rotate(-18 23 32)" /><ellipse cx="41" cy="32" rx="6" ry="9" fill="#243b53" transform="rotate(18 41 32)" /><path d="M27 47q5 3 10 0" fill="none" stroke="#243b53" strokeWidth="2.4" strokeLinecap="round" /></>) },
  { key: 'sun', bg: '#7fb8ff', el: (<><g stroke="#ffb300" strokeWidth="4" strokeLinecap="round"><path d="M32 4v7M32 53v7M4 32h7M53 32h7M12 12l5 5M47 47l5 5M52 12l-5 5M17 47l-5 5" /></g><circle cx="32" cy="32" r="16" fill="#ffd54a" />{eyes(30, 6)}{smile(37)}</>) },
  { key: 'cool', bg: '#ff9aa8', el: (<><circle cx="32" cy="34" r="21" fill="#ffd9b0" /><path d="M11 30q4-20 21-20t21 20q-21-8-42 0z" fill="#243b53" /><rect x="14" y="30" width="16" height="9" rx="4" fill="#243b53" /><rect x="34" y="30" width="16" height="9" rx="4" fill="#243b53" /><path d="M30 34h4" stroke="#243b53" strokeWidth="2.4" />{smile(46)}</>) },
];

export const avatarOf = (key) => AVATARS.find((a) => a.key === key);
