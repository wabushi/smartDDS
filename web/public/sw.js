const SHELL_CACHE = 'smartdds-shell-v1';
const SHELL_URLS = [
  '/',
  '/index.html',
  '/manifest.webmanifest',
  '/icons/icon-192.png',
  '/icons/icon-512.png'
];

self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(SHELL_CACHE)
      .then(cache => cache.addAll(SHELL_URLS))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(key => key.startsWith('smartdds-shell-') && key !== SHELL_CACHE).map(key => caches.delete(key))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', event => {
  const request = event.request;
  if (request.method !== 'GET' || new URL(request.url).origin !== self.location.origin) return;

  if (request.mode === 'navigate') {
    event.respondWith(
      fetch(request)
        .then(response => {
          const copy = response.clone();
          caches.open(SHELL_CACHE).then(cache => cache.put('/index.html', copy));
          return response;
        })
        .catch(() => caches.match('/index.html'))
    );
    return;
  }

  const staticDestinations = new Set(['script', 'style', 'image', 'font', 'manifest']);
  if (!staticDestinations.has(request.destination)) return;

  event.respondWith(
    caches.match(request).then(cached => cached || fetch(request).then(response => {
      if (response.ok) {
        const copy = response.clone();
        caches.open(SHELL_CACHE).then(cache => cache.put(request, copy));
      }
      return response;
    }))
  );
});

self.addEventListener('push', event => {
  let data = { title: 'SmartDDS', body: 'DDS notification' };
  try { data = event.data ? event.data.json() : data; } catch (_) {}
  event.waitUntil(self.registration.showNotification(data.title || 'SmartDDS', {
    body: data.body || '', icon: data.icon || '/icons/icon-192.png', data: data.data
  }));
});
self.addEventListener('notificationclick', event => {
  event.notification.close();
  event.waitUntil(clients.matchAll({ type: 'window' }).then(found => {
    if (found.length) return found[0].focus();
    return clients.openWindow(event.notification.data?.url || '/');
  }));
});
