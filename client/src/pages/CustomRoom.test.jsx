import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { StaticRouter } from 'react-router-dom/server';
import { describe, expect, it, vi } from 'vitest';
import { useSession } from '../net/session';
import CustomRoom from './CustomRoom.jsx';

vi.mock('../net/session', () => ({ useSession: vi.fn() }));

function renderRoom(room) {
  useSession.mockReturnValue({
    me: { userId: '1', name: 'Bách' },
    friends: [], room, connection: 'connected',
  });
  return renderToStaticMarkup(
    <StaticRouter location="/custom"><CustomRoom /></StaticRouter>,
  );
}

describe('Custom room setup', () => {
  it('offers all four playable modes, including UNO Draft', () => {
    const html = renderRoom(null);
    for (const mode of ['Classic', '2 vs 2', 'Side to Side', 'UNO Draft']) {
      expect(html).toContain(mode);
    }
  });

  it('displays only the code the server assigned', () => {
    expect(renderRoom(null)).not.toContain('class="room-code__label"');
    const html = renderRoom({
      code: 'AB12C3', mode: 'classic', hostSeat: 2,
      seats: [null, null, { seat: 2, userId: '1', name: 'Bách' }, null],
    });
    expect(html).toContain('<b class="display">AB12C3</b>');
    expect(html).toContain('Chủ phòng');
  });

  it('does not display a room that no longer contains the signed-in user', () => {
    const html = renderRoom({
      code: 'AB12C3', mode: 'classic', hostSeat: 0,
      seats: [{ seat: 0, userId: '2', name: 'Host' }, null, null, null],
    });
    expect(html).not.toContain('class="room-code__label"');
    expect(html).toContain('Tạo phòng');
  });
});
