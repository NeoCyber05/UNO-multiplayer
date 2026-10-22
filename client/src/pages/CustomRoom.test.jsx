import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { StaticRouter } from 'react-router-dom/server';
import { describe, expect, it } from 'vitest';
import { SocialProvider } from '../context/SocialContext.jsx';
import CustomRoom from './CustomRoom.jsx';

describe('Custom room setup', () => {
  it('offers all four playable modes, including UNO Draft', () => {
    const html = renderToStaticMarkup(
      <StaticRouter location="/custom"><SocialProvider><CustomRoom /></SocialProvider></StaticRouter>,
    );
    for (const mode of ['Classic', '2 vs 2', 'Side to Side', 'UNO Draft']) {
      expect(html).toContain(mode);
    }
  });

  it('displays a room code of at least six characters', () => {
    const html = renderToStaticMarkup(
      <StaticRouter location="/custom"><SocialProvider><CustomRoom /></SocialProvider></StaticRouter>,
    );
    expect(html.match(/<b class="display">([A-Z0-9]+)<\/b>/)?.[1].length).toBeGreaterThanOrEqual(6);
    expect(html).toContain('class="room-code__label"');
    expect(html).toContain('minLength="6"');
    expect(html).not.toContain('maxLength="4"');
  });
});
