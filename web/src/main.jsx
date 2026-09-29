import React, { useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import './styles.css';

const SERVICE = '8f6a0041-6d8e-4d44-9e7f-1a2b3c4d5e6f';
const COMMAND = '8f6a0042-6d8e-4d44-9e7f-1a2b3c4d5e6f';
const STATE = '8f6a0043-6d8e-4d44-9e7f-1a2b3c4d5e6f';
const RESPONSE = '8f6a0044-6d8e-4d44-9e7f-1a2b3c4d5e6f';
const encoder = new TextEncoder();
const decoder = new TextDecoder();
const BLE_COMMAND_SETTLE_MS = 250;
const BLE_WRITE_TIMEOUT_MS = 3000;
const MAX_FREQUENCY_MHZ = 99.99;
const WIFI_SSID_STORAGE_KEY = 'smartdds.wifiSsid';
const NOTIFICATION_HOURS_STORAGE_KEY = 'smartdds.notificationHours';
const PUSH_ENABLED_STORAGE_KEY = 'smartdds.pushEnabled';
const SERVICE_WORKER_URL = '/sw.js?v=20260830-5';
const MAX_NOTIFICATION_HOURS = 8760;
const WIFI_CONNECT_TIMEOUT_MS = 20000;
const WIFI_POLL_INTERVAL_MS = 5000;
const WIFI_REQUEST_TIMEOUT_MS = 4000;
const OUTPUTS = [
  { id: 's1', label: 'S1', name: 'SINE 1', description: 'Analog sine output 1' },
  { id: 's2', label: 'S2', name: 'SINE 2', description: 'Analog sine output 2' },
  { id: 's3', label: 'S3', name: 'SQUARE', description: 'Comparator/square output' },
];

function normalizeWaveform(outputId, value) {
  if (outputId === 's3') return 'square';
  return value === 'triangle' ? 'triangle' : 'sine';
}

function decodeBase64Url(value) {
  const base64 = value.replace(/-/g, '+').replace(/_/g, '/');
  const padded = base64 + '='.repeat((4 - (base64.length % 4)) % 4);
  const raw = atob(padded);
  return Uint8Array.from(raw, c => c.charCodeAt(0));
}

async function savePushSubscription(subscription, usageHours) {
  const response = await fetch('/push/subscribe', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      consecutive_usage_hours: usageHours,
      subscription: subscription.toJSON()
    })
  });
  const result = await response.json();
  if (!response.ok || !result.ok) {
    throw new Error(result.error || 'backend rejected the push subscription');
  }
  return result;
}

async function getOrCreatePushSubscription(registration) {
  const existing = await registration.pushManager.getSubscription();
  if (existing) return existing;

  const keyResponse = await fetch('/push/vapid-public-key', { cache: 'no-store' });
  const keyResult = await keyResponse.json();
  if (!keyResponse.ok || !keyResult.publicKey) {
    throw new Error(keyResult.error || 'backend VAPID key is unavailable');
  }
  return registration.pushManager.subscribe({
    userVisibleOnly: true,
    applicationServerKey: decodeBase64Url(keyResult.publicKey)
  });
}

function App() {
  const [device, setDevice] = useState(null);
  const [connected, setConnected] = useState(false);
  const [state, setState] = useState(null);
  const [frequencyValues, setFrequencyValues] = useState({ 0: '1.00', 1: '1.00' });
  const [phaseValues, setPhaseValues] = useState({ 0: '0.0', 1: '0.0' });
  const [waveform, setWaveform] = useState('sine');
  const [frequencyRegister, setFrequencyRegister] = useState('0');
  const [phaseRegister, setPhaseRegister] = useState('0');
  const [selectedOutput, setSelectedOutput] = useState('s1');
  const [actionStatus, setActionStatus] = useState('');
  const [log, setLog] = useState(['Ready. Use Chrome or Edge on localhost.']);
  const [pushStatus, setPushStatus] = useState('Not registered');
  const [pushSubscribed, setPushSubscribed] = useState(() => {
    try { return localStorage.getItem(PUSH_ENABLED_STORAGE_KEY) === 'true'; }
    catch (_) { return false; }
  });
  const [pushBusy, setPushBusy] = useState(false);
  const [notificationHours, setNotificationHours] = useState(() => {
    try {
      const saved = localStorage.getItem(NOTIFICATION_HOURS_STORAGE_KEY);
      const parsed = Number(saved);
      return saved && Number.isFinite(parsed) && parsed > 0 && parsed <= MAX_NOTIFICATION_HOURS ? saved : '1';
    } catch (_) { return '1'; }
  });
  const [bleStatus, setBleStatus] = useState('Ready to connect');
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [wifiSsid, setWifiSsid] = useState(() => {
    try { return localStorage.getItem(WIFI_SSID_STORAGE_KEY) || ''; }
    catch (_) { return ''; }
  });
  const [wifiPassword, setWifiPassword] = useState('');
  const [wifiSaving, setWifiSaving] = useState(false);
  const [wifiReachable, setWifiReachable] = useState(false);
  const [wifiChecking, setWifiChecking] = useState(true);
  const [installPrompt, setInstallPrompt] = useState(null);
  const [isInstalled, setIsInstalled] = useState(false);
  const [serverWifi, setServerWifi] = useState(null);
  const chars = useRef({});
  const sendChain = useRef(Promise.resolve());
  const wifiFailureCount = useRef(0);
  const wifiRefreshInFlight = useRef(null);
  const addLog = message => setLog(items => [message, ...items].slice(0, 8));

  const markWifiReachable = () => {
    wifiFailureCount.current = 0;
    setWifiReachable(true);
  };

  const markWifiFailure = () => {
    // One missed poll or command response is not evidence that station mode
    // disconnected. Require consecutive failures to avoid disabling controls
    // during a transient HTTP collision or server restart.
    wifiFailureCount.current += 1;
    if (wifiFailureCount.current >= 3) setWifiReachable(false);
  };

  const handleMessage = (message, updateState = true, announce = true) => {
    if (updateState && (message.type === 'state' || message.state)) {
      setState(message.state || message);
    }
    if (message.ok === false) {
      const error = message.error || 'DDS rejected the command';
      setActionStatus(`Error: ${error}`); addLog(`DDS error: ${error}`);
    } else if (announce) {
      const result = message.message || (message.type === 'state' ? 'State updated' : 'Response received');
      setActionStatus(result); addLog(result);
    }
    return message;
  };

  const handleJson = (data, updateState = true, announce = true) => {
    try { return handleMessage(JSON.parse(decoder.decode(data)), updateState, announce); }
    catch (_) { addLog('Received non-JSON BLE data'); return null; }
  };

  const requestWifiApi = async (path, options = {}) => {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), WIFI_REQUEST_TIMEOUT_MS);
    try {
      const response = await fetch(`/dds-api${path}`, { ...options, signal: controller.signal });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      return await response.json();
    } finally {
      clearTimeout(timer);
    }
  };

  const requestDeviceStatus = async () => {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), WIFI_REQUEST_TIMEOUT_MS);
    try {
      const response = await fetch(`/device-status?ts=${Date.now()}`, {
        cache: 'no-store',
        signal: controller.signal,
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      return await response.json();
    } finally {
      clearTimeout(timer);
    }
  };

  const refreshWifiState = async (announce = false) => {
    if (wifiRefreshInFlight.current) return wifiRefreshInFlight.current;
    setWifiChecking(true);
    const operation = (async () => {
      try {
        const deviceResult = await requestDeviceStatus();
        const reported = deviceResult?.device?.wifi;
        if (reported) setServerWifi(reported);
        if (!deviceResult?.connected) {
          markWifiFailure();
          if (announce) setActionStatus('Wi-Fi state refresh failed: device unavailable');
          return null;
        }

        const message = await requestWifiApi(`/api/state?ts=${Date.now()}`);
        markWifiReachable();
        handleMessage(message, true, announce);
        return message?.state || (message?.type === 'state' ? message : null);
      } catch (error) {
        markWifiFailure();
        if (announce) setActionStatus(`Wi-Fi state refresh failed: ${error.message}`);
        return null;
      } finally {
        setWifiChecking(false);
      }
    })();
    wifiRefreshInFlight.current = operation;
    try {
      return await operation;
    } finally {
      if (wifiRefreshInFlight.current === operation) wifiRefreshInFlight.current = null;
    }
  };

  const sendViaWifi = async payload => {
    const commandLabel = payload.cmd.replaceAll('_', ' ');
    setActionStatus(`Sending ${commandLabel} over Wi-Fi…`);
    try {
      const message = await requestWifiApi('/api/command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
      });
      markWifiReachable();
      if (message?.ok === false) {
        handleMessage(message, false, true);
        return false;
      }
      if (message?.queued) {
        setActionStatus(`Queued for ESP32 over Wi-Fi: ${commandLabel}`);
        addLog(`Queued ${commandLabel}; awaiting ESP32`);
      } else {
        handleMessage(message, true, true);
        setActionStatus(`Confirmed by ESP32 over Wi-Fi: ${commandLabel}`);
      }
      return true;
    } catch (error) {
      markWifiFailure();
      const message = error?.name === 'AbortError' ? 'request timed out' : (error?.message || String(error));
      setActionStatus(`Wi-Fi command failed: ${message}`);
      addLog(`Wi-Fi ${payload.cmd} failed: ${message}`);
      return false;
    }
  };

  const connect = async () => {
    if (!window.isSecureContext) {
      const message = 'Web Bluetooth requires HTTPS or localhost. Open the local app at https://192.168.50.180:5174/.';
      setBleStatus(message); addLog(message); return;
    }
    if (!navigator.bluetooth) {
      const message = 'Web Bluetooth is unavailable. Use Chrome or Edge on a device with Bluetooth.';
      setBleStatus(message); addLog(message); return;
    }
    setBleStatus('Choose AD9834-DDS in the browser Bluetooth picker…');
    let stage = 'picker';
    try {
      /* Filter by the advertised name as a fallback for phones that do not
       * expose service UUIDs until after connecting.  optionalServices grants
       * access to the custom service after the device is selected. */
      const selected = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: 'AD9834-DDS' }],
        optionalServices: [SERVICE],
      });
      stage = 'GATT connection';
      const server = await selected.gatt.connect();
      stage = 'DDS service discovery';
      let service;
      try {
        service = await server.getPrimaryService(SERVICE);
      } catch (error) {
        let discovered = [];
        try { discovered = await server.getPrimaryServices(); } catch (_) { /* diagnostic only */ }
        const listed = discovered.map(item => item.uuid).join(', ');
        const suffix = listed
          ? ` Discovered services: ${listed}`
          : ' The browser exposed no discoverable services.';
        throw new Error(`${error.message || 'service not found'}.${suffix}`);
      }
      stage = 'DDS characteristic discovery';
      // Discover the complete list once. Some Android Web Bluetooth stacks
      // report a generic GATT error when several UUID lookups are made.
      const discoveredCharacteristics = await service.getCharacteristics();
      const byUuid = new Map(discoveredCharacteristics.map(characteristic => [
        characteristic.uuid.toLowerCase(), characteristic,
      ]));
      const command = byUuid.get(COMMAND.toLowerCase());
      const stateChar = byUuid.get(STATE.toLowerCase());
      const response = byUuid.get(RESPONSE.toLowerCase());
      if (!command || !stateChar || !response) {
        const available = discoveredCharacteristics.map(characteristic => characteristic.uuid).join(', ');
        throw new Error(`DDS characteristics missing. Available: ${available || 'none'}`);
      }
      chars.current = { command, stateChar, response };
      addLog(
        `Command ready: write=${!!command.properties?.write}, ` +
        `no-response=${!!command.properties?.writeWithoutResponse}, ` +
        `methods=response:${typeof command.writeValueWithResponse}, ` +
        `no-response:${typeof command.writeValueWithoutResponse}, ` +
        `legacy:${typeof command.writeValue}`
      );
      stateChar.addEventListener('characteristicvaluechanged', event => handleJson(event.target.value));
      response.addEventListener('characteristicvaluechanged', event => handleJson(event.target.value));
      const readState = async () => {
        try {
          return await stateChar.readValue();
        } catch (stateError) {
          addLog(`State characteristic read failed; trying response (${stateError.message})`);
          return response.readValue();
        }
      };
      // Establish the useful read/write path before attempting CCCD
      // subscriptions. A few Android stacks report a generic GATT error for
      // startNotifications() and can otherwise make the following read fail.
      stage = 'state read';
      try {
        handleJson(await readState());
      } catch (readError) {
        addLog(`Initial state read unavailable; connection can still be used (${readError.message})`);
      }
      selected.addEventListener('gattserverdisconnected', () => {
        chars.current = {};
        setConnected(false);
        setBleStatus('Disconnected; press Find & pair DDS to reconnect');
        addLog('BLE disconnected');
      });
      setDevice(selected); setConnected(true); setBleStatus('Connected'); addLog(`Connected to ${selected.name || 'AD9834-DDS'}`);

      // Some Android Web Bluetooth implementations expose a valid READ/NOTIFY
      // characteristic but reject CCCD subscription with the unhelpful
      // "GATT Error Unknown". Notifications are an enhancement; reads and
      // command writes must remain usable when subscription is unavailable.
      for (const [name, characteristic] of [['state', stateChar], ['response', response]]) {
        try {
          stage = `${name} notifications`;
          await characteristic.startNotifications();
          addLog(`${name} notifications enabled`);
        } catch (notificationError) {
          addLog(`${name} notifications unavailable; using reads (${notificationError.message})`);
        }
      }
    } catch (error) {
      const detail = `${error.name || 'Error'}${error.message ? `: ${error.message}` : ''}`;
      const message = stage === 'picker' && error.name === 'NotFoundError'
        ? 'Bluetooth picker cancelled or no matching DDS was selected.'
        : `BLE ${stage} failed — ${detail}`;
      setBleStatus(message); addLog(message);
    }
  };

  const sendViaBle = async payload => {
    const bleUsable = connected && chars.current.command && device?.gatt?.connected;
    if (!bleUsable) {
      setActionStatus('Bluetooth is not connected');
      return false;
    }
    const commandLabel = payload.cmd.replaceAll('_', ' ');
    setActionStatus(`Sending ${commandLabel}…`);
    try {
      const commandData = encoder.encode(JSON.stringify(payload));
      const command = chars.current.command;
      addLog(`Writing ${payload.cmd} (${commandData.byteLength} bytes)`);
      const writeWithTimeout = (startOperation, label) => {
        let timer;
        const timeout = new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error(`${label} timed out; DDS did not acknowledge the write`)), BLE_WRITE_TIMEOUT_MS);
        });
        // Promise.race is deliberate here. Some Android Web Bluetooth builds
        // return a GATT promise that never settles when the connection has
        // gone stale; the UI must still leave "Sending" and recover.
        // Start the browser operation after the timeout has been installed.
        // Some Android builds expose incomplete `properties` metadata even
        // though the characteristic write method itself is usable.
        const operation = Promise.resolve().then(startOperation);
        return Promise.race([operation, timeout]).finally(() => clearTimeout(timer));
      };
      // Android Web Bluetooth can expose a write method and still reject that
      // mode with its generic GATT error. Try the other negotiated mode in
      // that case. A timeout is not retried because the command may have
      // reached the peripheral already.
      const writeAttempts = [];
      // nRF Connect's "Command" is Write Without Response. Prefer that
      // path because it is the native fast-control path for this command
      // characteristic; retain acknowledged-write fallbacks for browsers
      // and platforms that do not expose or permit it.
      if (typeof command.writeValueWithoutResponse === 'function') {
        writeAttempts.push([
          () => command.writeValueWithoutResponse(commandData),
          'Unacknowledged BLE write',
          'BLE write completed (without response)',
        ]);
      }
      if (typeof command.writeValueWithResponse === 'function') {
        writeAttempts.push([
          () => command.writeValueWithResponse(commandData),
          'Acknowledged BLE write',
          'BLE write completed (with response)',
        ]);
      }
      if (typeof command.writeValue === 'function') {
        writeAttempts.push([
          () => command.writeValue(commandData),
          'BLE write',
          'BLE write completed',
        ]);
      }
      if (!writeAttempts.length) {
        throw new Error('command characteristic has no usable write method');
      }

      let writeError;
      for (const [operation, label, successMessage] of writeAttempts) {
        try {
          await writeWithTimeout(operation, label);
          addLog(successMessage);
          writeError = null;
          break;
        } catch (error) {
          writeError = error;
          if (/timed out/i.test(error.message)) break;
          addLog(`${label} failed: ${error.message}; trying next mode`);
        }
      }
      if (writeError) throw writeError;
      addLog(`Sent ${payload.cmd}; waiting for ESP32 acknowledgement`);
      // The firmware deliberately queues writes outside the NimBLE GATT
      // callback.  Wait for that command task to apply the change before
      // reading state; an immediate read can return the previous state and
      // overwrite the user's newly selected value in the UI.
      await new Promise(resolve => setTimeout(resolve, BLE_COMMAND_SETTLE_MS));
      // Android Web Bluetooth may allow command writes while rejecting the
      // CCCD notification subscription. Keep the UI current by reading the
      // authoritative state after each command in that case (and harmlessly
      // refresh it even when notifications are enabled).
      let responseMessage = null;
      try {
        // Read the command acknowledgement first. The response characteristic
        // is now distinct from state, so errors cannot be mistaken for a stale
        // state read.
        try { responseMessage = handleJson(await chars.current.response.readValue(), false); }
        catch (responseError) { addLog(`Command response read failed: ${responseError.message}`); }
        if (responseMessage?.ok === false) return false;

        // The response is generated only after command_api_execute_json()
        // has changed the controller state. Use its snapshot immediately so
        // the page reflects the accepted command even if Android returns a
        // cached state characteristic value.
        if (responseMessage?.ok === true && responseMessage.state) {
          setState(responseMessage.state);
        }

        // The command response may contain a state snapshot from an earlier
        // queued command. Apply only the authoritative state characteristic.
        for (let attempt = 0; attempt < 3; attempt += 1) {
          try {
            handleJson(await chars.current.stateChar.readValue(), true);
            setActionStatus(`Confirmed by ESP32: ${commandLabel}`);
            return true;
          } catch (stateError) {
            if (attempt === 2) throw stateError;
            await new Promise(resolve => setTimeout(resolve, 100));
          }
        }
      } catch (readError) {
        if (responseMessage?.ok === true && responseMessage.state) {
          setActionStatus(`Acknowledged by ESP32: ${commandLabel}`);
          addLog(`State characteristic refresh unavailable; using command acknowledgement (${readError.message})`);
          return true;
        }
        addLog(`State refresh unavailable: ${readError.message}`);
      }
      return true;
    } catch (error) {
      const message = error?.message || String(error);
      setActionStatus(`Error: ${message}`);
      addLog(`Command error: ${message}`);
      if (/timed out|GATT|disconnected|not connected/i.test(message)) {
        chars.current = {};
        setConnected(false);
        setBleStatus('BLE write failed; reconnect to the DDS');
      }
      return false;
    }
  };

  const sendNow = async payload => {
    const wifiReady = wifiReachable && Boolean(state?.wifi?.connected);
    if (wifiReady) {
      const sent = await sendViaWifi(payload);
      if (sent) return true;
      addLog('Wi-Fi command failed; falling back to Bluetooth');
    }

    const bleUsable = connected && chars.current.command && device?.gatt?.connected;
    if (bleUsable) {
      if (!wifiReady) addLog('Wi-Fi unavailable; using Bluetooth');
      return sendViaBle(payload);
    }

    // The periodic Wi-Fi state can briefly be stale after the ESP obtains an
    // address. Try the API once before declaring both transports unavailable.
    if (!wifiReady && await sendViaWifi(payload)) return true;
    setActionStatus('Not connected: Wi-Fi unavailable and Bluetooth not connected');
    return false;
  };

  const queueOperation = operationFactory => {
    /* Web Bluetooth permits only one GATT transaction at a time on many
     * Android implementations. Queue button actions so a rapid control
     * change cannot overlap a write and its state refresh. */
    const operation = sendChain.current.then(operationFactory);
    sendChain.current = operation.catch(() => false);
    return operation;
  };

  const send = payload => queueOperation(() => sendNow(payload));
  const sendBluetooth = payload => queueOperation(() => sendViaBle(payload));

  const refreshDeviceStateNow = async (announce = true) => {
    if (wifiReachable && state?.wifi?.connected) {
      const snapshot = await refreshWifiState(announce);
      if (snapshot) return snapshot;
      addLog('Wi-Fi state refresh failed; falling back to Bluetooth');
    }
    if (connected && device?.gatt?.connected && chars.current.stateChar) {
      try {
        const message = handleJson(await chars.current.stateChar.readValue(), true, false);
        if (announce) setActionStatus('Device state refreshed from ESP32 over Bluetooth');
        return message?.state || (message?.type === 'state' ? message : null);
      } catch (error) {
        addLog(`Bluetooth state refresh failed: ${error.message}`);
      }
    }
    return refreshWifiState(announce);
  };

  const refreshDeviceState = (announce = true) => {
    // Serialize reads with writes. Android commonly fails overlapping Web
    // Bluetooth GATT operations with an unhelpful "GATT Error Unknown".
    const operation = sendChain.current.then(() => refreshDeviceStateNow(announce));
    sendChain.current = operation.catch(() => null);
    return operation;
  };

  const openWifiSettings = () => {
    const reportedSsid = state?.wifi?.ssid;
    if (reportedSsid) setWifiSsid(reportedSsid);
    setSettingsOpen(true);
    if (connected) void refreshDeviceState(false);
  };

  const saveWifi = async () => {
    const ssid = wifiSsid.trim();
    if (!connected) { setActionStatus('Error: connect to DDS before saving Wi-Fi'); return; }
    if (!ssid || ssid.length > 32) { setActionStatus('Error: SSID must be 1–32 characters'); return; }
    if (wifiPassword.length > 0 && (wifiPassword.length < 8 || wifiPassword.length > 63)) {
      setActionStatus('Error: password must be 8–63 characters, or empty for an open network'); return;
    }
    setWifiSaving(true);
    try {
    const serverUrl = window.location.origin;
      const ok = await sendBluetooth({ cmd: 'set_wifi', ssid, password: wifiPassword, server_url: serverUrl, connect: true });
      if (ok) {
        setWifiSsid(ssid);
        try { localStorage.setItem(WIFI_SSID_STORAGE_KEY, ssid); } catch (_) { /* optional convenience only */ }
        setActionStatus(`Wi-Fi credentials saved for ${ssid}; waiting for connection…`);

        const deadline = Date.now() + WIFI_CONNECT_TIMEOUT_MS;
        while (Date.now() < deadline) {
          await new Promise(resolve => setTimeout(resolve, 1000));
          const snapshot = await refreshDeviceState(false);
          if (snapshot?.wifi?.connected) {
            setWifiPassword('');
            setActionStatus(`Wi-Fi connected to ${snapshot.wifi.ssid || ssid}${snapshot.wifi.ip ? ` — ${snapshot.wifi.ip}` : ''}`);
            return;
          }
        }
        setActionStatus(`Credentials saved for ${ssid}, but Wi-Fi connection was not confirmed. Check the password and signal, then retry.`);
      }
    } finally { setWifiSaving(false); }
  };

  const clearWifi = async () => {
    if (!connected) { setActionStatus('Error: connect to DDS before clearing Wi-Fi'); return; }
    setWifiSaving(true);
    try {
      const ok = await sendBluetooth({ cmd: 'clear_wifi' });
      if (ok) {
        try { localStorage.removeItem(WIFI_SSID_STORAGE_KEY); } catch (_) { /* optional convenience only */ }
        setWifiSsid(''); setWifiPassword(''); setActionStatus('Saved Wi-Fi cleared');
      }
    } finally { setWifiSaving(false); }
  };

  useEffect(() => {
    void refreshWifiState(false);
    const timer = setInterval(() => { void refreshWifiState(false); }, WIFI_POLL_INTERVAL_MS);
    return () => clearInterval(timer);
  }, []);

  useEffect(() => {
    const standalone = window.matchMedia?.('(display-mode: standalone)').matches || window.navigator.standalone === true;
    setIsInstalled(standalone);

    const onBeforeInstallPrompt = event => {
      event.preventDefault();
      setInstallPrompt(event);
    };
    const onAppInstalled = () => {
      setIsInstalled(true);
      setInstallPrompt(null);
    };

    window.addEventListener('beforeinstallprompt', onBeforeInstallPrompt);
    window.addEventListener('appinstalled', onAppInstalled);
    return () => {
      window.removeEventListener('beforeinstallprompt', onBeforeInstallPrompt);
      window.removeEventListener('appinstalled', onAppInstalled);
    };
  }, []);

  useEffect(() => {
    if (!('serviceWorker' in navigator)) return undefined;
    navigator.serviceWorker.register(SERVICE_WORKER_URL, {
      scope: '/',
      type: 'classic',
      updateViaCache: 'none'
    }).catch(() => {});
    return undefined;
  }, []);

  useEffect(() => {
    if (!state?.wifi?.ssid) return;
    setWifiSsid(state.wifi.ssid);
    try { localStorage.setItem(WIFI_SSID_STORAGE_KEY, state.wifi.ssid); } catch (_) { /* optional convenience only */ }
  }, [state?.wifi?.ssid]);

  useEffect(() => {
    if (state) {
      const profile = state.outputs?.find(output => output.id === selectedOutput);
      const frequency0 = profile?.frequency0_hz ?? state.frequency0_hz ?? 0;
      const frequency1 = profile?.frequency1_hz ?? state.frequency1_hz ?? 0;
      const phase0 = profile?.phase0_deg ?? state.phase0_deg ?? 0;
      const phase1 = profile?.phase1_deg ?? state.phase1_deg ?? 0;
      setFrequencyValues({ 0: (Number(frequency0) / 1000000).toFixed(2), 1: (Number(frequency1) / 1000000).toFixed(2) });
      setPhaseValues({ 0: Number(phase0).toFixed(1), 1: Number(phase1).toFixed(1) });
      setFrequencyRegister(String(profile?.active_frequency_register ?? state.active_frequency_register ?? 0));
      setPhaseRegister(String(profile?.active_phase_register ?? state.active_phase_register ?? 0));
      setWaveform(normalizeWaveform(selectedOutput, profile?.waveform ?? state.waveform));
    }
  }, [state, selectedOutput]);

  useEffect(() => {
    let cancelled = false;

    const restorePushSubscription = async () => {
      if (!window.isSecureContext
          || !('serviceWorker' in navigator)
          || !('PushManager' in window)
          || !('Notification' in window)) return;

      if (Notification.permission === 'denied') {
        if (!cancelled) {
          setPushSubscribed(false);
          try { localStorage.removeItem(PUSH_ENABLED_STORAGE_KEY); } catch (_) {}
          setPushStatus('Notifications are blocked in browser settings');
        }
        return;
      }

      try {
        await navigator.serviceWorker.register(SERVICE_WORKER_URL, {
          scope: '/',
          type: 'classic',
          updateViaCache: 'none'
        });
        const readyRegistration = await navigator.serviceWorker.ready;
        if (Notification.permission !== 'granted') {
          if (!cancelled) {
            setPushSubscribed(false);
            try { localStorage.removeItem(PUSH_ENABLED_STORAGE_KEY); } catch (_) {}
            setPushStatus('Notifications have not been enabled on this browser');
          }
          return;
        }

        // Restore from the browser's real PushManager state first. Backend
        // synchronization must not make an existing browser subscription look
        // disabled after a page reload.
        const subscription = await getOrCreatePushSubscription(readyRegistration);
        if (!cancelled) {
          setPushSubscribed(true);
          setPushStatus('Notifications enabled; synchronizing schedule…');
          try { localStorage.setItem(PUSH_ENABLED_STORAGE_KEY, 'true'); } catch (_) {}
        }

        const storedHours = Number(localStorage.getItem(NOTIFICATION_HOURS_STORAGE_KEY));
        const usageHours = Number.isFinite(storedHours)
          && storedHours >= 0.1
          && storedHours <= MAX_NOTIFICATION_HOURS
          ? storedHours
          : 1;
        try {
          await savePushSubscription(subscription, usageHours);
        } catch (error) {
          if (!cancelled) {
            setPushStatus(`Notifications enabled in browser; backend sync failed: ${error.message}`);
          }
          return;
        }
        if (!cancelled) {
          setNotificationHours(String(usageHours));
          setPushSubscribed(true);
          setPushStatus(`Notifications active after ${usageHours} hour${usageHours === 1 ? '' : 's'} of consecutive output use`);
        }
      } catch (error) {
        if (!cancelled) {
          setPushSubscribed(false);
          try { localStorage.removeItem(PUSH_ENABLED_STORAGE_KEY); } catch (_) {}
          setPushStatus(`Push restore failed: ${error.name || 'Error'}: ${error.message}`);
        }
      }
    };

    void restorePushSubscription();
    return () => { cancelled = true; };
  }, []);

  const registerPush = async () => {
    const usageHours = Number(notificationHours);
    if (!Number.isFinite(usageHours) || usageHours <= 0 || usageHours > MAX_NOTIFICATION_HOURS) {
      setPushStatus(`Enter a usage duration between 0.1 and ${MAX_NOTIFICATION_HOURS} hours`);
      return;
    }
    if (!wifiControlAvailable) {
      setPushStatus('Notifications require a confirmed ESP32 Wi-Fi connection');
      return;
    }
    if (!window.isSecureContext) {
      setPushStatus('This browser does not trust the HTTPS certificate. Install the SmartDDS local CA, then reopen this page.');
      return;
    }
    if (!('serviceWorker' in navigator) || !('PushManager' in window) || !('Notification' in window)) {
      setPushStatus('Push is unavailable in this browser');
      return;
    }
    let browserSubscriptionCreated = false;
    try {
      setPushBusy(true);
      setPushStatus('Registering service worker…');
      const serviceWorkerResponse = await fetch(SERVICE_WORKER_URL, { cache: 'no-store' });
      const serviceWorkerType = serviceWorkerResponse.headers.get('content-type') || '';
      if (!serviceWorkerResponse.ok || !serviceWorkerType.includes('javascript')) {
        throw new Error(`service worker unavailable (HTTP ${serviceWorkerResponse.status}, ${serviceWorkerType || 'unknown type'})`);
      }
      const registration = await navigator.serviceWorker.register(SERVICE_WORKER_URL, {
        scope: '/',
        type: 'classic',
        updateViaCache: 'none'
      });
      await registration.update();
      const permission = await Notification.requestPermission();
      if (permission !== 'granted') {
        setPushSubscribed(false);
        try { localStorage.removeItem(PUSH_ENABLED_STORAGE_KEY); } catch (_) {}
        setPushStatus('Notification permission not granted');
        return;
      }
      setPushStatus('Loading push configuration…');
      const readyRegistration = await navigator.serviceWorker.ready;
      const subscription = await getOrCreatePushSubscription(readyRegistration);
      browserSubscriptionCreated = true;
      setPushSubscribed(true);
      try { localStorage.setItem(PUSH_ENABLED_STORAGE_KEY, 'true'); } catch (_) {}
      setPushStatus('Saving notification schedule…');
      await savePushSubscription(subscription, usageHours);
      try { localStorage.setItem(NOTIFICATION_HOURS_STORAGE_KEY, String(usageHours)); }
      catch (_) { /* Browser storage may be unavailable in private mode. */ }
      setPushSubscribed(true);
      setPushStatus(`Notifications active after ${usageHours} hour${usageHours === 1 ? '' : 's'} of consecutive output use`);
      addLog(`Push subscription saved (${usageHours} hour threshold)`);
    } catch (error) {
      if (!browserSubscriptionCreated) {
        setPushSubscribed(false);
        try { localStorage.removeItem(PUSH_ENABLED_STORAGE_KEY); } catch (_) {}
        setPushStatus(`Push error: ${error.name || 'Error'}: ${error.message}`);
      } else {
        setPushStatus(`Notifications enabled in browser; backend sync failed: ${error.message}`);
      }
    } finally {
      setPushBusy(false);
    }
  };

  const selected = OUTPUTS.find(output => output.id === selectedOutput) || OUTPUTS[0];
  const selectedProfile = state?.outputs?.find(output => output.id === selected.id);
  const reportedWifi = serverWifi?.connected
    ? serverWifi
    : state?.wifi?.connected
      ? state.wifi
      : (serverWifi || state?.wifi);
  const wifiConnected = Boolean(reportedWifi?.connected);
  const wifiControlAvailable = wifiConnected && wifiReachable;
  const notificationHoursNumber = Number(notificationHours);
  const notificationHoursValid = Number.isFinite(notificationHoursNumber)
    && notificationHoursNumber > 0
    && notificationHoursNumber <= MAX_NOTIFICATION_HOURS;
  const updateNotificationHours = value => {
    setNotificationHours(value);
    const parsed = Number(value);
    if (value !== '' && Number.isFinite(parsed) && parsed > 0 && parsed <= MAX_NOTIFICATION_HOURS) {
      try { localStorage.setItem(NOTIFICATION_HOURS_STORAGE_KEY, value); }
      catch (_) { /* Browser storage may be unavailable in private mode. */ }
    }
  };
  const pushEnabled = wifiControlAvailable && window.isSecureContext && notificationHoursValid && !pushBusy;
  const wifiAttempting = !wifiConnected && !wifiControlAvailable && (wifiChecking || Boolean(reportedWifi?.configured));
  const wifiIndicatorMode = wifiControlAvailable ? 'connected' : wifiConnected ? 'connecting' : wifiAttempting ? 'connecting' : 'offline';
  const wifiIndicatorText = wifiConnected
    ? `Wi-Fi connected${reportedWifi?.ssid ? ` · ${reportedWifi.ssid}` : ''}${reportedWifi?.ip ? ` · ${reportedWifi.ip}` : ''}${wifiControlAvailable ? '' : ' · control route unavailable'}`
    : wifiAttempting
      ? `Wi-Fi connecting${reportedWifi?.ssid ? ` · ${reportedWifi.ssid}` : ''}…`
      : 'Wi-Fi not configured';
  const bleControlAvailable = Boolean(connected && device?.gatt?.connected && chars.current.command);
  const controlsEnabled = bleControlAvailable || wifiControlAvailable;
  const wifiStatus = wifiConnected
    ? `Connected${reportedWifi?.ssid ? ` to ${reportedWifi.ssid}` : ''}${reportedWifi?.ip ? ` — ${reportedWifi.ip}` : ''}${wifiReachable ? '' : ' (web control route currently unreachable)'}`
    : reportedWifi?.configured
      ? `Saved${reportedWifi?.ssid ? ` for ${reportedWifi.ssid}` : ''}; connecting or reconnecting…`
      : wifiSsid
        ? `SSID ${wifiSsid} retained in this browser; connect Bluetooth to verify the ESP32`
        : 'Not configured';
  const wave = normalizeWaveform(selected.id, selectedProfile?.waveform || state?.waveform || waveform);
  const activeFrequencyRegister = selectedProfile?.active_frequency_register
    ?? state?.active_frequency_register ?? Number(frequencyRegister);
  const activePhaseRegister = selectedProfile?.active_phase_register
    ?? state?.active_phase_register ?? Number(phaseRegister);
  const freq = selectedProfile
    ? (activeFrequencyRegister ? selectedProfile.frequency1_hz : selectedProfile.frequency0_hz)
    : (state ? (activeFrequencyRegister ? state.frequency1_hz : state.frequency0_hz) : Number(frequencyValues[activeFrequencyRegister]) * 1000000);
  const phaseValue = selectedProfile
    ? (activePhaseRegister ? selectedProfile.phase1_deg : selectedProfile.phase0_deg)
    : (state ? (activePhaseRegister ? state.phase1_deg : state.phase0_deg) : phaseValues[activePhaseRegister]);
  const outputEnabled = selectedProfile?.output_enabled ?? state?.output_enabled ?? false;
  const outputWave = selected.id === 's3' ? 'SQUARE' : wave.toUpperCase();
  const outputFrequency = Number(freq);
  const formatFrequency = hz => {
    if (!Number.isFinite(hz)) return '—';
    return `${(hz / 1000000).toFixed(2)}MHZ`;
  };
  const applyFrequency = async registerArg => {
    const register = Number(registerArg ?? frequencyRegister);
    const entered = String(frequencyValues[register] ?? '').trim();
    if (!/^\d{1,2}(?:\.\d{0,2})?$/.test(entered)) {
      setActionStatus('Error: frequency must be entered as 0.00–99.99 MHz');
      return;
    }
    const mhz = Number(entered);
    if (!Number.isFinite(mhz) || mhz < 0 || mhz > MAX_FREQUENCY_MHZ) {
      setActionStatus('Error: frequency must be between 0.00 and 99.99 MHz');
      return;
    }
    const hz = Math.round(mhz * 1000000);
    if (await send({ cmd: 'set_frequency', output: selected.id, hz, register })) {
      setFrequencyValues(previous => ({ ...previous, [register]: mhz.toFixed(2) }));
      await send({ cmd: 'select_frequency', output: selected.id, register });
    }
  };
  const applyPhase = async registerArg => {
    const register = Number(registerArg ?? phaseRegister);
    const degrees = Number(phaseValues[register]);
    if (!Number.isFinite(degrees) || degrees < 0 || degrees >= 360) { setActionStatus('Error: phase must be 0–359.999°'); return; }
    if (await send({ cmd: 'set_phase', output: selected.id, degrees, register })) {
      setPhaseValues(previous => ({ ...previous, [register]: degrees.toFixed(1) }));
      await send({ cmd: 'select_phase', output: selected.id, register });
    }
  };
  const selectFrequencyRegister = async register => {
    setFrequencyRegister(String(register));
    await applyFrequency(register);
  };
  const selectPhaseRegister = async register => {
    setPhaseRegister(String(register));
    await applyPhase(register);
  };
  const applyWaveform = () => {
    const valid = selected.id === 's3'
      ? waveform === 'square'
      : waveform === 'sine' || waveform === 'triangle';
    if (!valid) {
      setActionStatus('Error: S1/S2 support sine or triangle; S3 supports square only');
      return;
    }
    return send({ cmd: 'set_waveform', output: selected.id, waveform });
  };
  const chooseOutput = output => {
    setSelectedOutput(output);
    if (controlsEnabled) void send({ cmd: 'select_output', output });
  };

  const installApp = async () => {
    if (!installPrompt) return;
    const promptEvent = installPrompt;
    setInstallPrompt(null);
    await promptEvent.prompt();
    await promptEvent.userChoice;
  };

  return <main>
    <header><div><span className="eyebrow">SMARTDDS / LOCAL CONTROL</span><h1>AD9834 <span>controller</span></h1><small className="build-id">Mobile push persistence build 2026-08-30.5</small></div><div className="transport-cluster"><div className="header-actions">{installPrompt && !isInstalled && <button onClick={installApp} className="install-button" type="button" aria-label="Install SmartDDS app">Install</button>}<button onClick={connect} className={`icon-button transport-button ble-indicator ${bleControlAvailable ? 'connected' : ''}`} aria-label="Bluetooth connect" title={bleControlAvailable ? 'Bluetooth connected' : 'Connect Bluetooth'}><span>BT</span><i className="status-dot" /></button><button onClick={openWifiSettings} className={`icon-button transport-button wifi-indicator ${wifiIndicatorMode}`} aria-label="Wi-Fi settings" title={wifiIndicatorText}><span>WiFi</span><i className="status-dot" /></button></div><small className={`transport-readout ${wifiIndicatorMode}`} role="status" aria-live="polite">{wifiIndicatorText}</small></div></header>
    <section className="mobile-push-card" aria-label="Usage notification settings">
      <h2>Usage notification</h2>
      <label className="notification-delay"><span>Notify after consecutive usage</span><span className="hours-input"><input type="number" min="0.1" max={MAX_NOTIFICATION_HOURS} step="0.1" inputMode="decimal" value={notificationHours} aria-label="Notification delay in hours" aria-invalid={!notificationHoursValid} onChange={event => updateNotificationHours(event.target.value)} /><strong>hours</strong></span></label>
      {!notificationHoursValid && <p className="field-error">Enter a value between 0.1 and {MAX_NOTIFICATION_HOURS} hours.</p>}
      <button onClick={registerPush} disabled={!pushEnabled}>{pushBusy ? 'Saving…' : pushSubscribed ? 'Save notification schedule' : 'Enable browser notifications'}</button>
      <p className="status" aria-live="polite">{!wifiControlAvailable ? 'Waiting for confirmed ESP32 Wi-Fi connection' : pushStatus}</p>
    </section>
    <div className="workspace-layout">
    <section className="output-tabs">{OUTPUTS.map(output => <button key={output.id} className={selectedOutput === output.id ? 'output-tab selected' : 'output-tab'} onClick={() => chooseOutput(output.id)}><strong>{output.label}</strong><span>{output.name}</span></button>)}</section>
    <div className="workspace-content">
    <section className="grid">
      <article className="panel controls" key={selected.id}>
        {!controlsEnabled && <div className="connection-lock" role="status"><strong>Controls locked</strong><span>{wifiConnected ? 'The ESP32 reports Wi-Fi connected, but its web control route is unreachable. Connect Bluetooth or check network routing.' : 'Connect Bluetooth to provision Wi-Fi, or make sure this server can reach the ESP32 over Wi-Fi.'}</span></div>}
        <fieldset disabled={!controlsEnabled}>
          <div className="register-section"><div className="register-row"><label>Frequency 0 (MHz)<input disabled={frequencyRegister !== '0'} value={frequencyValues[0]} onChange={e => { const value = e.target.value; if (/^\d{0,2}(?:\.\d{0,2})?$/.test(value)) setFrequencyValues(previous => ({ ...previous, 0: value })); }} onBlur={() => applyFrequency(0)} onKeyDown={e => { if (e.key === 'Enter') applyFrequency(0); }} inputMode="decimal" maxLength={5} /></label><label>Frequency 1 (MHz)<input disabled={frequencyRegister !== '1'} value={frequencyValues[1]} onChange={e => { const value = e.target.value; if (/^\d{0,2}(?:\.\d{0,2})?$/.test(value)) setFrequencyValues(previous => ({ ...previous, 1: value })); }} onBlur={() => applyFrequency(1)} onKeyDown={e => { if (e.key === 'Enter') applyFrequency(1); }} inputMode="decimal" maxLength={5} /></label></div><div className="register-toggle" role="group" aria-label="Active frequency register"><button type="button" className={frequencyRegister === '0' ? 'selected' : ''} onClick={() => selectFrequencyRegister(0)}>FREQ 0</button><button type="button" className={frequencyRegister === '1' ? 'selected' : ''} onClick={() => selectFrequencyRegister(1)}>FREQ 1</button></div></div>
          <div className="register-section"><div className="register-row"><label>Phase 0 (degrees)<input disabled={phaseRegister !== '0'} value={phaseValues[0]} onChange={e => { const value = e.target.value; if (/^\d{0,3}(?:\.\d{0,3})?$/.test(value)) setPhaseValues(previous => ({ ...previous, 0: value })); }} onBlur={() => applyPhase(0)} onKeyDown={e => { if (e.key === 'Enter') applyPhase(0); }} inputMode="decimal" /></label><label>Phase 1 (degrees)<input disabled={phaseRegister !== '1'} value={phaseValues[1]} onChange={e => { const value = e.target.value; if (/^\d{0,3}(?:\.\d{0,3})?$/.test(value)) setPhaseValues(previous => ({ ...previous, 1: value })); }} onBlur={() => applyPhase(1)} onKeyDown={e => { if (e.key === 'Enter') applyPhase(1); }} inputMode="decimal" /></label></div><div className="register-toggle" role="group" aria-label="Active phase register"><button type="button" className={phaseRegister === '0' ? 'selected' : ''} onClick={() => selectPhaseRegister(0)}>PHASE 0</button><button type="button" className={phaseRegister === '1' ? 'selected' : ''} onClick={() => selectPhaseRegister(1)}>PHASE 1</button></div></div>
          <label>Waveform<select value={waveform} onChange={e => setWaveform(normalizeWaveform(selected.id, e.target.value))}>{selected.id === 's3' ? <option value="square">Square</option> : <><option value="sine">Sine</option><option value="triangle">Triangle</option></>}</select></label>
          <button onClick={applyWaveform}>Apply waveform to {selected.label}</button>
          <div className="row"><button className="good" onClick={() => send({ cmd: 'set_output', output: selected.id, enabled: true })}>Output on</button><button className="danger" onClick={() => send({ cmd: 'set_output', output: selected.id, enabled: false })}>Output off</button></div>
        </fieldset>
        {actionStatus && <p className={actionStatus.startsWith('Error:') ? 'action-error' : 'action-status'}>{actionStatus}</p>}
      </article>
      <article className="panel state"><h2>{selected.label} programmed output</h2><div className="output-heading"><span>{selected.name}</span><strong>{outputEnabled ? 'ON' : 'OFF'}</strong></div><div className="hero-value">{formatFrequency(outputFrequency)}</div><div className="state-grid"><span>Waveform<strong>{outputWave}</strong></span><span>Phase<strong>{Number(phaseValue).toFixed(1)}°</strong></span><span>Register<strong>FREQ {activeFrequencyRegister}</strong></span></div><p className="muted">Configured/derived values only — physical output voltage and frequency require measurement hardware.</p></article>
    </section>
    <section className="lower">
      <article className="panel desktop-push-panel">
        <h2>Push notifications</h2>
        <p className="muted">Notifications are available after the ESP32 has a confirmed Wi-Fi connection. The timer runs while any DDS output is ON and resets when all outputs are OFF or the device remains unreachable.</p>
        <div className="notification-controls">
          <label className="notification-delay">
            <span>Notify after consecutive usage</span>
            <span className="hours-input"><input type="number" min="0.1" max={MAX_NOTIFICATION_HOURS} step="0.1" inputMode="decimal" value={notificationHours} aria-label="Notification delay in hours" aria-invalid={!notificationHoursValid} onChange={event => updateNotificationHours(event.target.value)} /><strong>hours</strong></span>
          </label>
        </div>
        {!notificationHoursValid && <p className="field-error">Enter a value between 0.1 and {MAX_NOTIFICATION_HOURS} hours.</p>}
        <p className="muted">Default: 1 hour. This preference is saved in this browser.</p>
        <p className="muted">First use on this device: <a href="/smartdds-ca.crt" download>install the SmartDDS HTTPS certificate</a>, mark it trusted, then fully reopen the browser.</p>
        <button onClick={registerPush} disabled={!pushEnabled} title={!wifiControlAvailable ? 'Connect the ESP32 to Wi-Fi first' : !window.isSecureContext ? 'Trust the SmartDDS HTTPS certificate first' : !notificationHoursValid ? 'Enter a valid usage duration first' : pushSubscribed ? 'Save the notification schedule' : 'Enable browser notifications'}>{pushBusy ? 'Saving…' : pushSubscribed ? 'Save notification schedule' : 'Enable browser notifications'}</button>
        <p className="status" aria-live="polite">{!wifiControlAvailable ? 'Waiting for confirmed ESP32 Wi-Fi connection' : !window.isSecureContext ? 'HTTPS certificate is not trusted by this browser' : !notificationHoursValid ? 'Enter a valid usage duration' : pushStatus}</p>
      </article>
      <article className="panel"><h2>Activity</h2><div className="log">{log.map((line, i) => <div key={i}>{line}</div>)}</div></article>
    </section>
    </div></div>
    {settingsOpen && <div className="modal-backdrop" role="presentation" onClick={() => setSettingsOpen(false)}><section className="settings-modal" role="dialog" aria-modal="true" aria-labelledby="wifi-title" onClick={event => event.stopPropagation()}><div className="modal-header"><div><span className="eyebrow">NETWORK SETTINGS</span><h2 id="wifi-title">ESP32 Wi-Fi</h2></div><button className="close-button" onClick={() => setSettingsOpen(false)} aria-label="Close">×</button></div><section className="mobile-notification-settings"><h2 className="settings-subheading">Usage notification</h2><label className="notification-delay"><span>Notify after consecutive usage</span><span className="hours-input"><input type="number" min="0.1" max={MAX_NOTIFICATION_HOURS} step="0.1" inputMode="decimal" value={notificationHours} aria-label="Notification delay in hours" aria-invalid={!notificationHoursValid} onChange={event => updateNotificationHours(event.target.value)} /><strong>hours</strong></span></label>{!notificationHoursValid && <p className="field-error">Enter a value between 0.1 and {MAX_NOTIFICATION_HOURS} hours.</p>}<button onClick={registerPush} disabled={!pushEnabled}>{pushBusy ? 'Saving…' : pushSubscribed ? 'Save notification schedule' : 'Enable browser notifications'}</button><p className="status" aria-live="polite">{pushStatus}</p></section><hr className="settings-divider" /><label>Wi-Fi SSID<input value={wifiSsid} onChange={event => setWifiSsid(event.target.value)} maxLength={32} autoComplete="ssid" /></label><label>Wi-Fi password<input type="password" value={wifiPassword} onChange={event => setWifiPassword(event.target.value)} maxLength={63} autoComplete="new-password" placeholder="Re-enter only when changing credentials" /></label><p className="muted">Credentials are sent over Bluetooth and stored in ESP32 flash. For security, the password is never returned by the ESP32 or retained by this page; an empty password is sent only when you press Save &amp; connect.</p><p className="wifi-status">Status: {wifiStatus}</p><div className="modal-actions"><button className="primary" onClick={saveWifi} disabled={wifiSaving || !bleControlAvailable}>{wifiSaving ? 'Saving…' : 'Save & connect'}</button><button onClick={clearWifi} disabled={wifiSaving || !bleControlAvailable}>Clear saved Wi-Fi</button><button onClick={() => setSettingsOpen(false)} disabled={wifiSaving}>Cancel</button></div></section></div>}
  </main>;
}

createRoot(document.getElementById('root')).render(<App />);
