import { ASSETS } from '../config/assets';
import playSfx from './playSfx';

/** Âm nút CALL UNO. volume 0-100. */
export default function playUno(volume = 80) {
  playSfx(ASSETS.sounds.uno, volume);
}
