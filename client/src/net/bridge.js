const RESPONSE_TIMEOUT_MS = 10000;
const MAX_REQUEST_ID = 2147483647;
const errorLabels = {
  BUSY: 'Người chơi hoặc phòng đang bận.',
  FORBIDDEN: 'Bạn không có quyền thực hiện thao tác này.',
  NOT_FOUND: 'Không tìm thấy phòng hoặc người chơi.',
  ROOM_FULL: 'Phòng đã đủ người.',
  STALE_TURN: 'Lượt đã thay đổi.',
  STORAGE_ERROR: 'Máy chủ chưa lưu được thay đổi.',
};

export function bridgeError(code, message = code) {
  return Object.assign(new Error(message), { code });
}

export function createBridge(target) {
  const pending = new Map();
  const subscribers = new Map();
  let active = false;
  let requestId = 0;
  const handler = () => target.webkit?.messageHandlers?.uno;

  function settle(id, error, payload) {
    const request = pending.get(id);
    if (!request) return;
    clearTimeout(request.timer);
    pending.delete(id);
    if (error) request.reject(error);
    else request.resolve(payload);
  }

  function receive(event) {
    const message = event.detail;
    if (message?.v !== 1) return;
    const { type, id, payload } = message;
    if (id === 0) {
      if (type === 'SERVER_SHUTDOWN' ||
          (type === 'CONNECTION_STATE' && payload?.status !== 'connected')) {
        for (const id of pending.keys()) settle(id, bridgeError('DISCONNECTED'));
      }
      subscribers.get(type)?.forEach((listener) => listener(payload));
      return;
    }
    if (type === 'OK') settle(id, null, payload);
    if (type === 'ERROR' || type === 'BAD_REQUEST') {
      const code = payload?.code ?? type;
      settle(id, bridgeError(code, errorLabels[code] ?? payload?.message));
    }
  }

  return {
    available: () => typeof handler()?.postMessage === 'function',
    start() {
      if (active) return;
      active = true;
      target.addEventListener('uno:message', receive);
    },
    stop() {
      if (!active) return;
      active = false;
      target.removeEventListener('uno:message', receive);
      for (const id of pending.keys()) settle(id, bridgeError('DISCONNECTED'));
    },
    subscribe(type, listener) {
      const listeners = subscribers.get(type) ?? new Set();
      subscribers.set(type, listeners);
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
        if (!listeners.size) subscribers.delete(type);
      };
    },
    request(type, payload = {}) {
      const native = handler();
      if (!native) return Promise.reject(bridgeError('NEED_DESKTOP', 'Mở UNO Desktop để kết nối.'));
      if (!active) return Promise.reject(bridgeError('DISCONNECTED'));
      if (requestId === MAX_REQUEST_ID) return Promise.reject(bridgeError('REQUEST_LIMIT'));
      const id = ++requestId;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => settle(id, bridgeError('TIMEOUT', 'Máy chủ chưa phản hồi.')), RESPONSE_TIMEOUT_MS);
        pending.set(id, { resolve, reject, timer });
        try {
          native.postMessage(JSON.stringify({ v: 1, id, type, payload }));
        } catch (error) {
          settle(id, error);
        }
      });
    },
  };
}
