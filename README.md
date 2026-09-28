# ESP32-C3 AD9834 DDS Controller

ESP-IDF firmware for an ESP32-C3 controlling an AD9834 over a conservative,
software-timed SPI-compatible 3-wire interface. The project keeps the AD9834
driver, authoritative programmed state, controller, command API, serial CLI,
BLE transport, display backend, and measurement abstraction separate.

## Hardware and wiring

| ESP32-C3 | AD9834 |
|---|---|
| GPIO 7 | FSY / FSYNC |
| GPIO 8 | SCK |
| GPIO 9 | SDA / serial data |
| GPIO 10 | RST |
| V5 | 5V |
| GND | GND |

`FS` and `PS` are not connected. Do not connect Sine1, Sine2, or Square
directly to an ESP32 GPIO or ADC. This firmware targets the common OLED-equipped
ESP32-C3 SuperMini variant: a 0.42-inch, 72x40 SSD1306-compatible I2C display
on SDA GPIO5, SCL GPIO6, address `0x3C`. The exact board revision should still
be confirmed against the physical PCB; startup probing reports whether a
display actually acknowledged.

The default AD9834 MCLK is 75 MHz. This is configurable; verify the module's
oscillator before treating programmed frequency as an actual measured output.
The AD9834 has no general-purpose digital telemetry readback: successful driver
writes are not proof of a physical waveform.

## Build, flash, and monitor

With ESP-IDF exported:

```bash
source /home/wabushi/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM0 monitor
```

The detected USB serial device is normally `/dev/ttyACM0`; prefer the stable
by-id path shown by `ls -l /dev/serial/by-id/` when available.

## Configuration

- `main/config/board_config.h`: DDS GPIOs and BLE name.
- `main/config/dds_config.h`: MCLK and serial clock (default 1 MHz).
- `main/display/display_hw.c`: SuperMini OLED I2C pins, address, and controller
  initialization.
- `sdkconfig.defaults`: ESP32-C3 target with Wi-Fi enabled. Wi-Fi credentials
  are configured at runtime from the web-panel gear icon or the serial CLI;
  they are stored in NVS and retried automatically after reboot.
- Display rotation defaults to enabled with a 10-second interval. Runtime CLI
  commands can change it.

## Serial CLI

The console prompt is `dds>`. Commands include:

```text
help | status | measure
freq <Hz> | freq0 <Hz> | freq1 <Hz>
select-freq 0|1
phase <degrees> | phase0 <degrees> | phase1 <degrees>
select-phase 0|1
wave sine|triangle|square
output on|off | reset | mclk [Hz]
display status | display next
display output sine1|sine2|square
display rotate on|off | display interval <seconds>
wifi status | wifi set <ssid> <password> | wifi clear
```

Frequency input accepts integer Hz and suffixes such as `38k`, `38kHz`, `1M`,
and `1MHz`. The initial startup state is 1 MHz, phase 0, sine, with the output
enabled. For the first oscilloscope test:

```text
freq 1000000
phase 0
wave sine
output on
status
```

## BLE

The peripheral advertises as `AD9834-DDS`. UUIDs are defined in
`main/transport/ble_service.h` and the implementation provides a custom
service with a write command characteristic and a readable/notifiable state
characteristic. Command writes use JSON such as:

```json
{"cmd":"set_frequency","hz":1000000}
{"cmd":"set_phase","degrees":90.0}
{"cmd":"set_waveform","waveform":"sine"}
{"cmd":"set_output","enabled":true}
{"cmd":"get_state"}
{"cmd":"set_wifi","ssid":"MyNetwork","password":"secretpass","connect":true}
{"cmd":"clear_wifi"}
```

BLE callbacks call the same command API as the serial CLI. The command, state,
and response characteristics are readable/notifiable without requiring link
encryption during discovery, which keeps Android Web Bluetooth compatible. The
browser/OS may still show a system pairing prompt depending on its BLE security
policy. This is not application identity: any permitted BLE client can connect.

The current UUIDs are:

| Item | UUID |
|---|---|
| Service | `8f6a0041-6d8e-4d44-9e7f-1a2b3c4d5e6f` |
| Command (write) | `8f6a0042-6d8e-4d44-9e7f-1a2b3c4d5e6f` |
| State (read/notify) | `8f6a0043-6d8e-4d44-9e7f-1a2b3c4d5e6f` |
| Response (read/notify) | `8f6a0044-6d8e-4d44-9e7f-1a2b3c4d5e6f` |

State and command results are JSON. Measurements are represented by `null`
until measurement hardware exists. The local React/Web Bluetooth client is in
`web/` and uses these same characteristics.

Wi-Fi credentials are sent over the existing command characteristic. The
password is never included in state or status responses. SSIDs may be 1–32
bytes; WPA/WPA2 passwords must be 8–63 bytes (an empty password requests an
open network). `connect:true` causes the ESP32 to connect immediately and the
saved credentials are loaded automatically on the next boot.

## Local web control panel

```bash
cd web
npm install
npm run dev
```

The development server listens on all local network interfaces over HTTPS.
From a phone on the same LAN, open:

```text
https://192.168.50.180:5174/
```

The server certificate is signed by the local SmartDDS development CA. On each
phone/computer, open `/smartdds-ca.crt` from the control-panel URL, install it as
a CA certificate, explicitly trust it, and fully reopen the browser. Merely
continuing past a certificate warning is not enough for service workers. If the
PC's LAN IP changes, regenerate the server certificate with the new IP in its
subject-alternative names and restart the server.

Select **Find & pair DDS** in Chrome on Android. Web Bluetooth requires HTTPS,
Bluetooth enabled on the phone, and a browser that supports Web Bluetooth. Most
iOS browsers currently do not expose Web Bluetooth, even when the page uses
HTTPS. The phone connects to the ESP32 over BLE; the web server only serves the
control page over the LAN.

The development server certificate, CA, and private keys are stored in
`.local-certs/`; only the public CA certificate is downloadable from the site.
Never distribute either private key. Use a publicly trusted certificate or
reverse proxy when deploying outside the local development network.

The production bundle can be checked with:

```bash
npm run build
npm run preview
```

Use the gear icon after connecting to open Wi-Fi settings. Enter the SSID and
password, then choose **Save & connect**. **Clear saved Wi-Fi** removes the
credentials from the ESP32. The app includes frequency, phase, waveform, output control, live state
notifications, and a display-only warning that values are programmed rather
than measured. It does not claim physical waveform verification.

## Push notifications and backend deployment

`web/public/sw.js` is a service-worker foundation for browser notifications.
The notification control remains disabled until the ESP32 reports a successful
Wi-Fi connection, its HTTP control endpoint is reachable through this server,
and the page is running in a browser-trusted HTTPS secure context.
The local app can request notification permission and create a Web Push
subscription when `VITE_PUSH_PUBLIC_KEY` contains the backend's URL-safe VAPID
public key. It does not send push messages locally: a future backend must store
subscriptions and send signed VAPID notifications using its private key. The
ESP32 is not expected to contact browser push services directly.

The notification panel includes a consecutive-usage threshold in hours,
defaulting to 1 hour and saved in the current browser. Once VAPID/backend
registration is configured, the subscription metadata includes this threshold
as `consecutive_usage_hours`. Actual usage tracking and timed notification
delivery remain backend work; the browser preference alone does not schedule a
push message.

When ready to deploy, serve the built `web/dist` directory from the backend or
static hosting, add an HTTPS origin, provide the VAPID public key at build
time, and add a backend endpoint to receive/store the subscription JSON. The
backend can later expose HTTP/WebSocket APIs that translate into the same
command API. The ESP32 currently supports station-mode Wi-Fi connection, while
network command transport remains a future addition.

## Display and measurements

The display subsystem models Sine1, Sine2, and Square as derived views of one
DDS state, not three independent DDS generators. It performs an `EREZ` OLED
startup test, then rotates non-blockingly every 10 seconds and updates
immediately when state changes. Displayed values are programmed/derived state,
not independent measurements.

The measurement subsystem deliberately returns unavailable values until safe
analog circuitry is designed. Future work includes attenuation, buffering,
biasing, input protection, bandwidth selection, peak/RMS measurement, and
frequency measurement. Programmed frequency, Vpp, and voltage must not be
reported as measured values.

## Future transports

The Wi-Fi station manager is implemented, and a future network transport should
attach to `command_api`, just as serial and BLE do today. A future mobile app can use the documented BLE service to control
frequency, phase, waveform, output, and register selection and read programmed
state plus measured state once measurement hardware exists.
