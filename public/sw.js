const VERSION = '3.7-splash';
const APP_SHELL_CACHE = 'voicebridge-shell-' + VERSION;
const APP_SHELL_FILES = [
  '/',
  '/index.html',
  '/manifest.json',
  '/icon-192.png',
  '/icon-512.png',
  '/apple-touch-icon.png'
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(APP_SHELL_CACHE)
      .then(cache => cache.addAll(APP_SHELL_FILES))
      .catch(() => {})
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(
        keys
          .filter(key => key.startsWith('voicebridge-shell-') && key !== APP_SHELL_CACHE)
          .map(key => caches.delete(key))
      ))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') return;
  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return;
  if (!APP_SHELL_FILES.includes(url.pathname) && url.pathname !== '/') return;

  event.respondWith(
    caches.match(request).then(cached => {
      const fresh = fetch(request)
        .then(response => {
          if (response && response.ok) {
            const copy = response.clone();
            caches.open(APP_SHELL_CACHE).then(cache => cache.put(request, copy));
          }
          return response;
        })
        .catch(() => cached);
      return cached || fresh;
    })
  );
});

function pushText(value) {
  return value || '';
}

function notificationOptions(data) {
  const isSos = data.type === 'help_request';
  const actions = [];

  if ((data.actions || []).includes('answer')) {
    actions.push({ action: 'answer', title: '\u041e\u0442\u0432\u0435\u0442\u0438\u0442\u044c' });
  }
  if ((data.actions || []).includes('remind')) {
    actions.push({ action: 'remind', title: '\u041e\u0442\u043b\u043e\u0436\u0438\u0442\u044c' });
  }

  return {
    body: pushText(data.body),
    tag: data.tag || data.type || 'voicebridge',
    renotify: true,
    requireInteraction: Boolean(data.requireInteraction || isSos),
    icon: '/icon-192.png',
    badge: '/icon-192.png',
    data: {
      url: data.url || '/',
      type: data.type || 'voicebridge'
    },
    actions
  };
}

self.addEventListener('push', (event) => {
  let data = {};
  try {
    data = event.data ? event.data.json() : {};
  } catch {
    data = {
      title: 'VoiceBridge',
      body: '\u041d\u043e\u0432\u043e\u0435 \u0443\u0432\u0435\u0434\u043e\u043c\u043b\u0435\u043d\u0438\u0435'
    };
  }

  const title = data.title || 'VoiceBridge';
  event.waitUntil(self.registration.showNotification(title, notificationOptions(data)));
});

async function focusOrOpen(url, action) {
  const allClients = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
  const targetUrl = new URL(url, self.location.origin).href;

  for (const client of allClients) {
    if ('focus' in client) {
      client.postMessage({ type: 'voicebridge_push_action', action });
      await client.focus();
      if ('navigate' in client) return client.navigate(targetUrl);
      return client;
    }
  }

  if (self.clients.openWindow) return self.clients.openWindow(targetUrl);
}

self.addEventListener('notificationclick', (event) => {
  event.notification.close();

  const baseUrl = event.notification.data?.url || '/';
  const url = new URL(baseUrl, self.location.origin);

  const type = event.notification.data?.type;
  let action = 'answer';
  if (event.action === 'remind') {
    action = 'remind';
    url.searchParams.set('pushAction', 'remind');
  } else if (type === 'help_request') {
    action = 'sos';
    url.searchParams.set('pushAction', 'sos');
    url.searchParams.set('clearReminder', '1');
  } else {
    url.searchParams.set('pushAction', 'answer');
    url.searchParams.set('clearReminder', '1');
  }
  url.searchParams.set('pushTs', Date.now().toString());

  event.waitUntil(focusOrOpen(url.href, action));
});
