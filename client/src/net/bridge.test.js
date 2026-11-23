import { afterEach, expect, it, vi } from 'vitest';
import { createBridge } from './bridge';

afterEach(() => vi.useRealTimers());

it('settles a legacy BAD_REQUEST reply from the current dispatcher', async () => {
  const { bridge, emit, post } = setupBridge();
  bridge.start();
  const result = bridge.request('FRIEND_LIST');
  const { id } = JSON.parse(post.mock.calls[0][0]);
  emit({ id, type: 'BAD_REQUEST', payload: {} });
  await expect(result).rejects.toMatchObject({ code: 'BAD_REQUEST' });
  bridge.stop();
});

function setupBridge() {
  const target = new EventTarget();
  const post = vi.fn();
  target.webkit = { messageHandlers: { uno: { postMessage: post } } };
  const emit = (message) => target.dispatchEvent(Object.assign(new Event('uno:message'), {
    detail: { v: 1, ...message },
  }));
  return { bridge: createBridge(target), emit, post };
}

it('resolves the matching response and rejects errors without resending', async () => {
  const { bridge, emit, post } = setupBridge();
  bridge.start();
  const first = bridge.request('DRAW', { turnNo: 3 });
  const second = bridge.request('PLAY', { turnNo: 4 });
  const [draw, play] = post.mock.calls.map(([json]) => JSON.parse(json));
  emit({ id: play.id, type: 'ERROR', payload: { code: 'STALE_TURN' } });
  emit({ id: draw.id, type: 'OK', payload: { accepted: true } });
  await expect(first).resolves.toEqual({ accepted: true });
  await expect(second).rejects.toMatchObject({ code: 'STALE_TURN' });
  expect(post).toHaveBeenCalledTimes(2);
  bridge.stop();
});

it('cleans up remounts and expires pending requests', async () => {
  vi.useFakeTimers();
  const { bridge, emit } = setupBridge();
  const onPush = vi.fn();
  const unsubscribe = bridge.subscribe('GAME_STATE', onPush);
  bridge.start();
  bridge.stop();
  bridge.start();
  bridge.start();
  emit({ id: 0, type: 'GAME_STATE', payload: {} });
  expect(onPush).toHaveBeenCalledTimes(1);
  unsubscribe();
  const rejected = expect(bridge.request('HELLO')).rejects.toMatchObject({ code: 'TIMEOUT' });
  await vi.advanceTimersByTimeAsync(10000);
  await rejected;
  bridge.stop();
});
