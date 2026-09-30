import Modal from '../Modal.jsx';
import { IconBook, IconClose } from '../Icons.jsx';

const SECTIONS = [
  {
    title: 'Mục tiêu',
    text: 'Đánh hết bài trên tay trước người khác. Khi còn 1 lá phải bấm UNO, quên hô sẽ bị phạt rút 2 lá.',
  },
  {
    title: 'Lượt đánh',
    text: 'Đánh 1 lá cùng màu, cùng số hoặc cùng ký hiệu với lá trên cùng. Không có lá hợp lệ thì rút 1 lá. Mỗi lượt có 20 giây.',
  },
  {
    title: 'Lá chức năng',
    text: 'Skip: người kế mất lượt. Reverse: đảo chiều. +2: người kế rút 2 lá. Discard All: bỏ luôn mọi lá cùng màu trên tay. Wild: đổi màu. Wild +4: đổi màu và người kế rút 4 lá.',
  },
  {
    title: 'Classic',
    text: '4 người, mỗi người một mình. Người hết bài đầu tiên thắng.',
  },
  {
    title: '2 vs 2',
    text: 'Hai đội, đồng đội ngồi đối diện và luôn nhìn thấy bài của nhau. Một người trong đội hết bài là cả đội thắng.',
  },
  {
    title: 'Side to Side',
    text: 'Hai đội, đồng đội ngồi cạnh nhau (bên trái bạn) và nhìn thấy bài của nhau.',
  },
  {
    title: 'ELO',
    text: 'Classic, 2 vs 2 và Side to Side là trận xếp hạng: thắng được cộng ELO, thua bị trừ. Phòng Custom không tính ELO.',
  },
];

export default function RulesModal({ open, onClose }) {
  return (
    <Modal open={open} onClose={onClose} label="Luật chơi" className="lobby-modal">
      <div className="lobby-modal__head">
        <span className="display lobby-modal__title"><IconBook size={24} /> Luật chơi</span>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Đóng"><IconClose /></button>
      </div>
      <div className="rules">
        {SECTIONS.map((s) => (
          <section key={s.title} className="rules__item">
            <h3 className="display">{s.title}</h3>
            <p>{s.text}</p>
          </section>
        ))}
      </div>
    </Modal>
  );
}
