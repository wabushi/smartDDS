self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('push', event => {
  let data = { title: 'SmartDDS', body: 'DDS notification' };
  try { data = event.data ? event.data.json() : data; } catch (_) {}
  event.waitUntil(self.registration.showNotification(data.title || 'SmartDDS', {
    body: data.body || '', icon: data.icon, data: data.data
  }));
});
self.addEventListener('notificationclick', event => {
  event.notification.close();
  event.waitUntil(clients.matchAll({ type: 'window' }).then(found => {
    if (found.length) return found[0].focus();
    return clients.openWindow(event.notification.data?.url || '/');
  }));
});
