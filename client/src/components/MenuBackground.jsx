import { ASSETS } from '../config/assets';

/** Nền các màn hình menu (ảnh sinh bởi scripts/gen_menu_bg.py). */
export default function MenuBackground() {
  return <div className="menu-bg" style={{ backgroundImage: `url(${ASSETS.backgrounds.menu})` }} aria-hidden />;
}
