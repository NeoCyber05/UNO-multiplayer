import { expect, it } from 'vitest';
import { validDisplayName } from './displayName';

it('uses the server limit in Unicode code points', () => {
  expect(validDisplayName('a'.repeat(16))).toBe(true);
  expect(validDisplayName('🃏'.repeat(16))).toBe(true);
  expect(validDisplayName('🃏'.repeat(17))).toBe(false);
  expect(validDisplayName('a'.repeat(17))).toBe(false);
  expect(validDisplayName('  ')).toBe(false);
  expect(validDisplayName('A\nB')).toBe(false);
});
