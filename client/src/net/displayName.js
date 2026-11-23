const MAX_NAME_CODE_POINTS = 16;
export const INVALID_DISPLAY_NAME = 'Tên hiển thị phải có từ 1 đến 16 ký tự hợp lệ.';

export function validDisplayName(value) {
  const name = value.trim();
  const length = [...name].length;
  return length > 0 && length <= MAX_NAME_CODE_POINTS && !/[\u0000-\u001f\u007f\ud800-\udfff]/u.test(name);
}
