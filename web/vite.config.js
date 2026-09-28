import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import fs from 'node:fs';
import http from 'node:http';
import webpush from 'web-push';

const registryPort = Number(process.env.DDS_REGISTRY_PORT || 5175);
const configuredDeviceIps = String(process.env.DDS_DEVICE_IP || '192.168.60.178')
  .split(',')
  .map(value => value.trim())
  .filter(Boolean);
const publicCaCertificate = fs.readFileSync(new URL('./public/smartdds-ca.crt', import.meta.url));
const runtimeDirectory = new URL('./.runtime/', import.meta.url);
const pushStateFile = new URL('./.runtime/push-state.json', import.meta.url);

fs.mkdirSync(runtimeDirectory, { recursive: true, mode: 0o700 });

const loadPushState = () => {
  try {
    const saved = JSON.parse(fs.readFileSync(pushStateFile, 'utf8'));
    if (saved.vapid?.publicKey && saved.vapid?.privateKey) return saved;
  } catch (_) { /* Generate the initial local push configuration below. */ }
  const vapid = webpush.generateVAPIDKeys();
  return {
    vapid,
    subscriptions: [],
    usage: { activeSince: null, lastActiveAt: null, notificationSent: false },
  };
};

const pushState = loadPushState();
const savePushState = () => {
  fs.writeFileSync(pushStateFile, `${JSON.stringify(pushState, null, 2)}\n`, { mode: 0o600 });
};
savePushState();
webpush.setVapidDetails('mailto:smartdds@localhost', pushState.vapid.publicKey, pushState.vapid.privateKey);

function ddsRegistry() {
  let device = null;
  let pollPromise = null;
  const candidates = new Set(configuredDeviceIps);
  const isIpv4 = value => /^(?:\d{1,3}\.){3}\d{1,3}$/.test(value) &&
    value.split('.').every(part => Number(part) <= 255);
  const current = () => device && Date.now() - device.lastSeen < 60000 ? device : null;
  const json = (response, status, payload) => {
    response.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    response.end(JSON.stringify(payload));
  };
  const readJson = request => new Promise((resolve, reject) => {
    let body = '';
    request.on('data', chunk => {
      if (body.length < 131072) body += chunk;
    });
    request.on('end', () => {
      try { resolve(JSON.parse(body || '{}')); } catch (error) { reject(error); }
    });
    request.on('error', reject);
  });
  const sendDueNotifications = async () => {
    if (!pushState.usage.activeSince) return;
    const elapsedMs = Date.now() - pushState.usage.activeSince;
    const survivors = [];
    for (const entry of pushState.subscriptions) {
      if (entry.notified || elapsedMs < entry.hours * 3600000) {
        survivors.push(entry);
        continue;
      }
      try {
        await webpush.sendNotification(entry.subscription, JSON.stringify({
          title: 'SmartDDS usage alert',
          body: `A DDS output has been enabled continuously for ${entry.hours} hour${entry.hours === 1 ? '' : 's'}.`,
          data: { url: '/' },
        }));
        entry.notified = true;
        survivors.push(entry);
        console.log(`[PUSH] Sent ${entry.hours}-hour consecutive-usage notification`);
      } catch (error) {
        if (error.statusCode !== 404 && error.statusCode !== 410) {
          survivors.push(entry);
          console.error(`[PUSH] Delivery failed: ${error.message}`);
        }
      }
    }
    pushState.subscriptions = survivors;
    savePushState();
  };
  const updateUsage = async state => {
    const now = Date.now();
    if (!state) {
      const offlineForMs = pushState.usage.lastActiveAt
        ? now - pushState.usage.lastActiveAt
        : 0;
      if (pushState.usage.activeSince && offlineForMs >= 20000) {
        pushState.usage = { activeSince: null, lastActiveAt: null, notificationSent: false };
        for (const entry of pushState.subscriptions) entry.notified = false;
        savePushState();
        console.log('[PUSH] Consecutive DDS usage timer reset after device became unreachable');
      }
      return;
    }
    const outputs = Array.isArray(state?.outputs) ? state.outputs : [];
    const active = outputs.some(output => output.output_enabled === true) ||
      (outputs.length === 0 && state?.output_enabled === true);
    if (active) {
      if (!pushState.usage.activeSince) {
        pushState.usage.activeSince = now;
        for (const entry of pushState.subscriptions) entry.notified = false;
        console.log('[PUSH] Consecutive DDS usage timer started');
      }
      pushState.usage.lastActiveAt = now;
      await sendDueNotifications();
    } else if (pushState.usage.activeSince) {
      pushState.usage = { activeSince: null, lastActiveAt: null, notificationSent: false };
      for (const entry of pushState.subscriptions) entry.notified = false;
      savePushState();
      console.log('[PUSH] Consecutive DDS usage timer reset');
    }
  };
  const probe = ip => new Promise(resolve => {
    const request = http.get({ hostname: ip, port: 80, path: '/api/state', timeout: 2500 }, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => {
        if (body.length < 32768) body += chunk;
      });
      response.on('end', () => {
        try {
          const state = JSON.parse(body);
          resolve(response.statusCode === 200 && state && typeof state === 'object'
            ? { name: 'AD9834-DDS', ip, lastSeen: Date.now(), source: 'poll', wifi: state.wifi, state }
            : null);
        } catch (_) {
          resolve(null);
        }
      });
    });
    request.on('timeout', () => request.destroy());
    request.on('error', () => resolve(null));
  });
  const pollCandidates = () => {
    if (pollPromise) return pollPromise;
    pollPromise = (async () => {
      for (const ip of candidates) {
        if (!isIpv4(ip)) continue;
        const found = await probe(ip);
        if (found) {
          const changed = !device || device.ip !== found.ip || device.source !== 'poll';
          device = found;
          if (changed) console.log(`[DDS] ESP32 verified over Wi-Fi at ${device.ip}`);
          return found;
        }
      }
      return null;
    })().finally(() => { pollPromise = null; });
    return pollPromise;
  };

  return {
    name: 'smartdds-device-registry',
    configureServer(server) {
      const registry = http.createServer((request, response) => {
        if (request.method === 'POST' && request.url === '/api/device/register') {
          let body = '';
          request.on('data', chunk => {
            if (body.length < 2048) body += chunk;
          });
          request.on('end', () => {
            try {
              const report = JSON.parse(body);
              if (!isIpv4(report.ip)) return json(response, 400, { ok: false, error: 'invalid device IP' });
              candidates.add(report.ip);
              // Preserve the last full state obtained from /api/state. The
              // heartbeat contains only reachability data; replacing the
              // record with it made device.wifi disappear until the next
              // poll, so the browser's Wi-Fi indicator blinked every cycle.
              const previousWifi = device?.ip === report.ip ? device.wifi : null;
              const previousState = device?.ip === report.ip ? device.state : null;
              device = {
                name: report.device || 'AD9834-DDS',
                ip: report.ip,
                lastSeen: Date.now(),
                source: 'report',
                wifi: {
                  ...(previousWifi || {}),
                  connected: report.connected !== false,
                  ip: report.ip,
                },
                state: previousState,
              };
              console.log(`[DDS] ESP32 registered at ${device.ip}`);
              return json(response, 200, { ok: true });
            } catch (_) {
              return json(response, 400, { ok: false, error: 'invalid JSON' });
            }
          });
          return;
        }
        json(response, 404, { ok: false, error: 'not found' });
      });
      registry.listen(registryPort, '0.0.0.0', () => {
        console.log(`[DDS] Device registration listening on http://0.0.0.0:${registryPort}`);
      });
      void pollCandidates().then(found => updateUsage(found?.state));
      const pollTimer = setInterval(async () => {
        const found = await pollCandidates();
        await updateUsage(found?.state);
      }, 5000);
      server.httpServer?.once('close', () => {
        clearInterval(pollTimer);
        registry.close();
      });

      server.middlewares.use('/smartdds-ca.crt', (_request, response) => {
        response.writeHead(200, {
          'Content-Type': 'application/x-x509-ca-cert',
          'Content-Disposition': 'attachment; filename="smartdds-ca.crt"',
          'Cache-Control': 'no-store',
        });
        response.end(publicCaCertificate);
      });
      server.middlewares.use('/device-status', async (_request, response) => {
        const active = current() || await pollCandidates();
        json(response, 200, { connected: Boolean(active), device: active });
      });
      server.middlewares.use('/push/vapid-public-key', (request, response) => {
        if (request.method !== 'GET') return json(response, 405, { ok: false, error: 'method not allowed' });
        json(response, 200, { publicKey: pushState.vapid.publicKey });
      });
      server.middlewares.use('/push/status', (request, response) => {
        if (request.method !== 'GET') return json(response, 405, { ok: false, error: 'method not allowed' });
        json(response, 200, {
          configured: true,
          subscriptions: pushState.subscriptions.length,
          usageActive: Boolean(pushState.usage.activeSince),
          activeSince: pushState.usage.activeSince,
          elapsedSeconds: pushState.usage.activeSince
            ? Math.floor((Date.now() - pushState.usage.activeSince) / 1000)
            : 0,
        });
      });
      server.middlewares.use('/push/subscribe', async (request, response) => {
        if (request.method !== 'POST') return json(response, 405, { ok: false, error: 'method not allowed' });
        try {
          const body = await readJson(request);
          const hours = Number(body.consecutive_usage_hours);
          const subscription = body.subscription;
          if (!Number.isFinite(hours) || hours < 0.1 || hours > 8760) {
            return json(response, 400, { ok: false, error: 'usage duration must be between 0.1 and 8760 hours' });
          }
          if (!subscription?.endpoint || !subscription?.keys?.p256dh || !subscription?.keys?.auth) {
            return json(response, 400, { ok: false, error: 'invalid push subscription' });
          }
          const existing = pushState.subscriptions.find(entry => entry.subscription.endpoint === subscription.endpoint);
          if (existing) {
            existing.subscription = subscription;
            existing.hours = hours;
            existing.notified = false;
          } else {
            pushState.subscriptions.push({ subscription, hours, notified: false, createdAt: Date.now() });
          }
          savePushState();
          json(response, 200, { ok: true, hours, usageActive: Boolean(pushState.usage.activeSince) });
        } catch (_) {
          json(response, 400, { ok: false, error: 'invalid JSON' });
        }
      });
      server.middlewares.use('/dds-api', async (request, response) => {
        const active = current() || await pollCandidates();
        if (!active) return json(response, 503, { ok: false, error: 'ESP32 has not registered or is offline' });
        // Do not forward the browser's Origin/Referer/Sec-Fetch/Cookie header set.
        // The ESP-IDF HTTP server has a deliberately bounded request parser and
        // the DDS API only needs the request body metadata.
        const upstreamHeaders = {
          host: active.ip,
          connection: 'close',
        };
        if (request.headers['content-type']) {
          upstreamHeaders['content-type'] = request.headers['content-type'];
        }
        if (request.headers['content-length']) {
          upstreamHeaders['content-length'] = request.headers['content-length'];
        }
        const proxy = http.request({
          hostname: active.ip,
          port: 80,
          method: request.method,
          path: request.url,
          headers: upstreamHeaders,
          timeout: 4000,
        }, upstream => {
          active.lastSeen = Date.now();
          response.writeHead(upstream.statusCode || 502, upstream.headers);
          upstream.pipe(response);
        });
        proxy.on('timeout', () => proxy.destroy(new Error('ESP32 request timed out')));
        proxy.on('error', error => json(response, 502, { ok: false, error: error.message }));
        request.pipe(proxy);
      });
    },
  };
}

export default defineConfig({
  plugins: [react(), ddsRegistry()],
  server: {
    host: '0.0.0.0',
    port: 5174,
    https: {
      // Keep private TLS material outside Vite's served project root.
      key: fs.readFileSync(new URL('../.local-certs/lan-key.pem', import.meta.url)),
      cert: fs.readFileSync(new URL('../.local-certs/lan-cert.pem', import.meta.url))
    }
  }
});
