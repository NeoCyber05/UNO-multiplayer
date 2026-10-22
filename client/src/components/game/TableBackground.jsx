import { ASSETS } from '../../config/assets';

/**
 * Nền bàn chơi: ảnh ASSETS.backgrounds.table (gradient, chữ UNO, dải màu, confetti, vignette)
 * làm lớp dưới, phía trên là tia sáng xoay + đèn sân khấu động bằng CSS và mặt bàn.
 */
export default function TableBackground() {
  return (
    <div className="table-bg" style={{ backgroundImage: `url(${ASSETS.backgrounds.table})` }} aria-hidden>
      <div className="table-bg__rays" />
      <div className="table-bg__spot table-bg__spot--l" />
      <div className="table-bg__spot table-bg__spot--r" />
      {/* Mặt bàn elip — người chơi ngồi quanh mép, bài đặt trên mép bàn */}
      <div className="table-felt" />
    </div>
  );
}
