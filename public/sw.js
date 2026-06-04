const VERSION = '3.1-push';

self.addEventListener('install', () => self.skipWaiting());

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.map(key => caches.delete(key))))
      .then(() => self.clients.claim())
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

async function focusOrOpen(url) {
  const allClients = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
  const targetUrl = new URL(url, self.location.origin).href;

  for (const client of allClients) {
    if ('focus' in client) {
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

  if (event.action === 'answer') url.searchParams.set('pushAction', 'answer');
  if (event.action === 'remind') url.searchParams.set('pushAction', 'remind');
  if (!event.action && event.notification.data?.type === 'help_request') {
    url.searchParams.set('pushAction', 'sos');
  }

  event.waitUntil(focusOrOpen(url.href));
});
