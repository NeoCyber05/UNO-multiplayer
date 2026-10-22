import { useLayoutEffect, useState } from 'react';

export const STAGE_W = 1440;
export const STAGE_H = 900;

/**
 * Khung thiết kế 1440x900, tự co giãn vừa cửa sổ như game.
 * Scale theo cạnh vừa khít; cạnh còn lại được kéo dài thêm (tính theo đơn vị thiết kế)
 * để phủ kín cửa sổ, không để thừa dải trống khi tỉ lệ màn hình khác 16:10.
 */
export default function Stage({ children }) {
  const [box, setBox] = useState({ scale: 1, w: STAGE_W, h: STAGE_H });

  useLayoutEffect(() => {
    const fit = () => {
      const scale = Math.min(window.innerWidth / STAGE_W, window.innerHeight / STAGE_H);
      setBox({ scale, w: window.innerWidth / scale, h: window.innerHeight / scale });
    };
    fit();
    window.addEventListener('resize', fit);
    return () => window.removeEventListener('resize', fit);
  }, []);

  return (
    <div className="stage-viewport">
      <div className="stage" style={{ width: box.w, height: box.h, transform: `scale(${box.scale})` }}>
        {children}
      </div>
    </div>
  );
}
