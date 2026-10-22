import { rankOf } from '../data/ranks';

export default function RankBadge({ elo, rank = rankOf(elo), size = 36, label = false }) {
  return <span className="rank-badge" style={{ color: rank.color }}>
    <img src={`/images/ranks/${rank.key}.svg`} width={size} height={size} alt={label ? '' : rank.name} />
    {label && <span>{rank.name}</span>}
  </span>;
}
