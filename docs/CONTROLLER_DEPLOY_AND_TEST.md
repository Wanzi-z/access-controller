# Controller Deploy and Test Runbook

This is the current complete loop for programming an Access Controller,
proving AP recovery, switching between two Wi-Fi networks, validating the
`open-automation.org/devices` punch/proxy path, and running the automated tests.

Use this runbook when a new board is plugged in, after firmware/UI changes, or
before an install handoff.

## Known Endpoints

- Repo: `/home/andy/projects/access-controller`
- Firmware: `code/controller`
- Controller AP: `http://192.168.4.1/`
- Controller AP password: `pyfitech`
- Local Device Manager: `http://192.168.1.40:8102/`
- Public punch/control front door: `https://open-automation.org/devices`
- Sonic Device Manager checkout: `ssh sonic`, `/home/andy/projects/device-manager`

The current public policy is intentionally split:

- `POST https://open-automation.org/devices` is open for device punch-in.
- Browser/UI `GET https://open-automation.org/devices/` requires Basic auth.
- The unauthenticated public root must not expose the controller UI.

## 1. Build Firmware

```bash
cd /home/andy/projects/access-controller/code/controller
source /home/andy/esp/esp-idf/export.sh
idf.py build
```

Pass evidence:

- `build/controller.bin` exists.
- `controller.bin` fits in the smallest app partition.
- Build warnings are only known legacy warnings, not new errors in changed
  files.

## 2. Identify and Program the Controller

Find the USB serial adapter:

```bash
ls -l /dev/ttyUSB* /dev/ttyACM*
```

For the CP2102N ESP32-S3 boards used in July 2026, this DTR/RTS sequence was
the reliable way to enter download mode without touching the buttons:

```bash
python3 - <<'PY'
import time
import serial
ser = serial.Serial('/dev/ttyUSB0', 115200)
for dtr, rts, delay in [
    (False, True, 0.1),
    (True, True, 0.1),
    (True, False, 0.3),
    (False, False, 0.3),
]:
    ser.dtr = dtr
    ser.rts = rts
    time.sleep(delay)
ser.close()
PY
```

Flash the built image:

```bash
cd /home/andy/projects/access-controller/code/controller/build
source /home/andy/esp/esp-idf/export.sh
python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 \
  --before no_reset --after no_reset write_flash "@flash_args"
```

If the board does not enter download mode, use the manual sequence: hold
`BOOT`, tap `RESET`, release `BOOT`, then run the flash command.

## 3. First-Boot AP Check

If the board has no saved Wi-Fi or Wi-Fi/server validation fails, it should
serve an AP named `ac_<uuid-suffix>`.

```bash
nmcli dev wifi rescan
nmcli -f SSID,BSSID,SIGNAL,SECURITY dev wifi list | grep '^ac_'
```

Connect and query state:

```bash
nmcli dev wifi connect <BSSID_OR_SSID> password pyfitech ifname wlp0s20f3
curl -sf http://192.168.4.1/api/state | jq '{uuid:.device.uuid, ap:.device.network.wifi_ap_ip, sta:.device.network.wifi_sta_ip, server:.server.url}'
```

Pass evidence:

- `wifi_ap_ip` is `192.168.4.1`.
- `wifi_sta_ip` is `null` or empty.
- The UUID is stable for the board.
- The Settings page can be opened at `http://192.168.4.1/`.

## 4. Provision Wi-Fi and Verify Server Punch

Save a network from AP mode:

```bash
curl -sS -X POST http://192.168.4.1/api/wifi/add \
  -H 'Content-Type: application/json' \
  --data '{"ssid":"Echo42","password":"qscwdvpk"}'
```

The request may time out because the controller reboots immediately after
saving. Reconnect the host to the same Wi-Fi and find the controller by serial
logs, Device Manager, or a LAN scan.

Verify direct state:

```bash
DEVICE_URL=http://<controller_sta_ip>
curl -sf "$DEVICE_URL/api/state" | jq '{uuid:.device.uuid, ssid:.wifi.active_ssid, ip:.device.network.wifi_sta_ip, quality:.device.network.wifi_sta_quality, server:.server.url, ota:.system.firmware.otaState}'
```

Verify the public punch route:

```bash
curl -sS -i -X POST https://open-automation.org/devices \
  -H 'Content-Type: application/json' \
  -d '{"id":"route-smoke","name":"Route Smoke","type":"access_controller"}'
curl -sS -i https://open-automation.org/devices/ | sed -n '1,12p'
```

Expected:

- `POST /devices` returns `200 application/json`.
- `GET /devices/` returns `401 Basic realm="Device Manager"` unless
  credentials are supplied.
- No unauthenticated public request returns the full UI.

If you create a temporary route-smoke record, remove it from
`sonic:/home/andy/projects/device-manager/data/discovery/punched.json` with the
same privileges that own the Device Manager data volume.

## 5. Switch Between Two Saved Networks

Store both networks first. Example:

```bash
curl -sS -X POST "$DEVICE_URL/api/wifi/add" \
  -H 'Content-Type: application/json' \
  --data '{"ssid":"Echo42","password":"qscwdvpk"}'

curl -sS -X POST "$DEVICE_URL/api/wifi/add" \
  -H 'Content-Type: application/json' \
  --data '{"ssid":"HelloWorld","password":"qscwdvpk"}'
```

Connect to a saved network:

```bash
curl -sS -X POST "$DEVICE_URL/api/wifi/connect" \
  -H 'Content-Type: application/json' \
  --data '{"ssid":"HelloWorld"}'
```

Wait for the reboot and verify the new IP/SSID through one of:

```bash
curl -sf http://<new_ip>/api/state | jq '{ssid:.wifi.active_ssid, ip:.device.network.wifi_sta_ip, quality:.device.network.wifi_sta_quality}'
curl -sf 'http://192.168.1.40:8102/api/devices/<device-manager-id>/access-controller/state' \
  | jq '{uuid:.state.device.uuid, ssid:.state.wifi.active_ssid, ip:.state.device.network.wifi_sta_ip, quality:.state.device.network.wifi_sta_quality}'
```

Repeat in the other direction. Pass evidence is not just that the SSID changes:
the active-network card must show live link quality, RSSI, STA IP, gateway, STA
MAC, AP BSSID, channel, and security for the currently connected AP.

## 6. AP Fallback and Recovery

To prove AP fallback, make the active Wi-Fi unavailable. Practical options:

- Turn off the phone hotspot currently used by the controller.
- Turn off the active AP/router.
- Save a deliberately unreachable network, then reboot.

Expected behavior:

1. The controller fails station connection or server-policy validation.
2. AP mode starts and `ac_<uuid-suffix>` appears.
3. `http://192.168.4.1/api/state` responds.
4. The recovery loop periodically retries saved station credentials.
5. When a valid saved network comes back, the controller leaves AP mode and
   returns to station mode.

During AP mode, run:

```bash
cd /home/andy/projects/access-controller/code/controller/tests
DEVICE_URL=http://192.168.4.1 npm run test:quick
```

After recovery, run:

```bash
curl -sf "$DEVICE_URL/api/state" | jq '{ssid:.wifi.active_ssid, ap:.device.network.wifi_ap_ip, sta:.device.network.wifi_sta_ip, server:.server}'
```

Pass evidence:

- AP UI/API is available while station is unavailable.
- Saved-network recovery returns to station mode without serial intervention.
- `server.requireReachable=true` keeps station mode only when
  `https://open-automation.org/devices` accepts the punch.

## 7. Device Manager and Reverse Proxy

Check Device Manager locally:

```bash
curl -sf http://192.168.1.40:8102/api/health
```

If it is hung or the UI is unavailable:

```bash
ssh sonic 'docker restart device-manager'
ssh sonic 'systemctl status device-manager-tunnel.service --no-pager -l'
ssh sonic 'systemctl status esp32-tunnel.service --no-pager -l'
```

Find the controller in Device Manager:

```bash
curl -sf http://192.168.1.40:8102/api/devices | jq '.devices[] | select(.type=="access_controller") | {id, name, ip, last_seen}'
```

Verify the access-controller state route:

```bash
curl -sf 'http://192.168.1.40:8102/api/devices/<device-manager-id>/access-controller/state' \
  | jq '{uuid:.state.device.uuid, ssid:.state.wifi.active_ssid, ip:.state.device.network.wifi_sta_ip, quality:.state.device.network.wifi_sta_quality, ota:.state.system.firmware.otaState}'
```

Pass evidence:

- Device Manager health returns `{"ok":true}`.
- The controller UUID in Device Manager matches direct `/api/state`.
- Device Manager can read Wi-Fi link metrics through the tunnel/proxy path.
- Public `GET /devices/` is authenticated, not open.

## 8. OTA Deploy

Direct controller OTA:

```bash
curl -sS --max-time 180 -X POST "$DEVICE_URL/api/ota/upload" \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Firmware-Filename: controller.bin' \
  --data-binary @/home/andy/projects/access-controller/code/controller/build/controller.bin
```

Device Manager OTA:

```bash
curl -sS --max-time 180 -X POST \
  "http://192.168.1.40:8102/api/devices/<device-manager-id>/access-controller/ota" \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Firmware-Filename: controller.bin' \
  --data-binary @/home/andy/projects/access-controller/code/controller/build/controller.bin
```

Wait for the reboot and verify:

```bash
for i in $(seq 1 30); do
  curl -sf "$DEVICE_URL/api/state" | jq '{running:.system.firmware.runningPartition.label, ota:.system.firmware.otaState, uptime:.system.uptimeSeconds}' && break
  sleep 2
done
```

Pass evidence:

- Upload returns `{"ok":true,"reboot":true}`.
- Running app partition flips to the alternate slot.
- OTA state becomes `valid`.
- Direct state and Device Manager state agree after reboot.

## 9. Automated Test Gate

Run the full automated suite after deploy:

```bash
cd /home/andy/projects/access-controller/code/controller/tests
DEVICE_URL="$DEVICE_URL" npm test
```

For smaller targeted checks:

```bash
DEVICE_URL="$DEVICE_URL" npm run test:api
DEVICE_URL="$DEVICE_URL" npm run test:ui
DEVICE_URL="$DEVICE_URL" npm run test:quick
```

The full suite is expected to run build verification, connectivity, API state,
API configuration, stress/bulk operations, and Playwright UI tests. The normal
suite may include intentional skips for interactive-only browser subtests, but
must have zero failures.

Normal passing tests should not beep. Keep quiet-test mode enabled for routine
load/test traffic; use audible alerts only for unexpected failures or reboots.

## 10. UI Smoke Gate

For settings/network work, verify the rendered UI, not only the API:

```bash
cd /home/andy/projects/access-controller
DEVICE_URL=http://<controller-ip> node - <<'NODE'
const { chromium } = require('./code/controller/tests/node_modules/playwright');
const required = ['LINK QUALITY', 'STA IP', 'GATEWAY', 'STA MAC', 'AP BSSID', 'CHANNEL', 'SECURITY'];
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: '/snap/bin/chromium' });
  for (const [name, viewport] of Object.entries({ desktop: { width: 1440, height: 1200 }, mobile: { width: 390, height: 1200 } })) {
    const page = await browser.newPage({ viewport });
    await page.goto(process.env.DEVICE_URL + '/?v=' + Date.now(), { waitUntil: 'domcontentloaded', timeout: 30000 });
    await page.getByRole('button', { name: 'Settings' }).click();
    await page.waitForSelector('#wifiActive .wifi-detail', { timeout: 15000 });
    const text = (await page.locator('#wifiActive').innerText()).toUpperCase();
    for (const label of required) {
      if (!text.includes(label)) throw new Error(`${name}: missing ${label}`);
    }
    await page.screenshot({ path: `/tmp/access-controller-settings-wifi-${name}.png`, fullPage: false });
    await page.close();
  }
  await browser.close();
})();
NODE
```

Inspect the screenshots before calling the UI done.

## 11. Aux Keypad Feedback Gate

Run this gate after changing lock, exit, motion, keypad, fob, Wiegand, buzzer,
or MCP23017 behavior.

Current bench note: as of 2026-07-10, only the Wiegand keypad on CH0 is
installed/audible. The firmware still pulses both keypad push outputs for aux
input feedback, so a CH2 test is expected to beep the installed CH0 keypad.

First prove the direct feedback path:

```bash
curl -sS --max-time 8 -X POST "$DEVICE_URL/api/buzzer/error-beep" \
  -H 'Content-Type: application/json' \
  -d '{"channel":0,"beeps":1}'
```

Expected: one keypad beep. Channel `0` means both keypad push outputs.

If the direct path is questionable, isolate one push line without touching
locks or input handlers:

```bash
curl -sS --max-time 10 -X POST "$DEVICE_URL/api/keypad/push-test" \
  -H 'Content-Type: application/json' \
  -d '{"channel":1,"pulses":1,"activeMs":750,"idleMs":500,"activeHigh":true}'
```

The production pulse is 750 ms active. A 100 ms push was observed to be too
short and flaky for the installed keypad.

Then prove each real handler path. Each request should produce one audible
keypad beep:

```bash
for endpoint in exit motion keypad fob; do
  for channel in 1 2; do
    curl -sS --max-time 10 -X POST "$DEVICE_URL/api/$endpoint" \
      -H 'Content-Type: application/json' \
      -d "{\"channel\":$channel,\"enable\":true,\"alert\":true,\"delay\":4,\"test\":true}"
    sleep 1
  done
done
```

Pass evidence:

- Direct `channel:0` beep works.
- Exit CH1 and CH2 tests beep.
- Motion CH1 and CH2 tests beep.
- Keypad CH1 and CH2 tests beep.
- FOB CH1 and CH2 tests beep.
- Auto re-arm does not create extra keypad beeps.
- If only CH0 has a keypad installed, CH2 tests are still expected to be
  audible through CH0 because aux feedback pulses both keypad push outputs.

## 2026-07-08 Known-Good Evidence

- Programmed ESP32-S3 MAC `b8:f8:62:cb:9a:a0`.
- Controller UUID: `acce5501-b8f8-42cb-9aa0-05c0085e08b7`.
- AP SSID observed after flash: `ac_08b7`.
- Provisioned to Echo42: controller IP `10.69.136.23`.
- Device Manager ID used in verification:
  `61c08cc8-5225-545e-b987-5a0be207b871`.
- OTA uploaded `build/controller.bin` over direct controller API; rebooted into
  `app1`; OTA state `valid`.
- Live Wi-Fi state after OTA exposed STA IP, gateway, RSSI, quality, channel,
  security, and AP BSSID.
- Device Manager health returned `{"ok":true,"service":"device-manager"}`.
- Device Manager access-controller state returned matching UUID, Echo42 SSID,
  STA IP, Wi-Fi quality, and OTA state.

## 2026-07-10 Keypad Feedback and Regression Evidence

- Firmware commit: `5b518d4 Make keypad feedback reliable for aux inputs`.
- Controller IP during validation: `10.69.136.23` on `Echo42`.
- OTA state after install: `valid`.
- Direct `POST /api/buzzer/error-beep` with `{"channel":0,"beeps":1}` beeped.
- Exit CH1, Exit CH2, Motion CH1, Motion CH2, Keypad CH1, Keypad CH2, FOB CH1,
  and FOB CH2 test actions beeped the installed CH0 keypad.
- CH0 isolated push testing showed 300 ms did not beep and 750 ms did beep, so
  production keypad push timing is 750 ms active.
- CH2 direct push was not audible on the bench because no keypad was installed
  on CH2. Aux feedback still pulses CH2 push output along with CH0.
- Public `POST https://open-automation.org/devices` returned `200`.
- Public `GET https://open-automation.org/devices/` returned `401 Basic
  realm="Device Manager"`.
- Full test suite:
  `DEVICE_URL=http://10.69.136.23 npm test` returned `161 passed, 0 failed,
  2 skipped`.
- Additional targeted suites:
  `npm run test:api` returned `77 passed, 0 failed`; `npm run test:ui`
  returned `51 passed, 0 failed, 2 skipped`.
- Playwright desktop/mobile settings smoke passed and showed the active network
  card with link quality, RSSI, STA IP, gateway, STA MAC, AP BSSID, channel,
  and security.
