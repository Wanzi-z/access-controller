---
name: access-controller-reliability
description: Run full reliability, load, OTA, AP recovery, and UI uptime validation for the Access Controller ESP32.
---

# Access Controller Reliability Procedure

Use this skill before considering controller firmware/UI changes stable. It assumes the repo root is `~/projects/access-controller` and the live controller is reachable at `DEVICE_URL` or AP fallback `http://192.168.4.1`.

## Preconditions

- Build from a clean or intentionally dirty tree:
  ```bash
  cd ~/projects/access-controller/code/controller
  source ~/esp/esp-idf/export.sh
  idf.py build
  ```
- Confirm the live device:
  ```bash
  curl -sf "$DEVICE_URL/api/state" | jq '{version:.system.firmware.projectVersion, ota:.system.firmware.otaState, network:.device.network, uptime:.system.uptimeSeconds}'
  ```
- If serial flash/AP recovery is required, confirm the USB serial port:
  ```bash
  ls -l /dev/ttyUSB* /dev/ttyACM*
  ```

## Complete Deploy And Test Loop

For new boards, network changes, OTA work, UI changes, or install handoff, use
the full runbook in `docs/CONTROLLER_DEPLOY_AND_TEST.md`. Do not stop at a
local build or a direct-controller curl when the user asked for end-to-end
proof.

Minimum sequence:

1. Build firmware with ESP-IDF.
2. Program the controller over USB or OTA.
3. Prove first-boot or fallback AP mode at `http://192.168.4.1/`.
4. Provision Wi-Fi from AP mode.
5. Save and test two station networks when available, normally `Echo42` and
   `HelloWorld`; switch both directions and confirm the active SSID, IP, link
   quality/RSSI, gateway, AP BSSID, channel, and security update in `/api/state`
   and the Settings UI.
6. Prove AP fallback by disabling the active Wi-Fi or using unreachable saved
   credentials. Confirm `ac_<uuid-suffix>` appears, `192.168.4.1` responds, and
   recovery returns to station mode when a valid saved network is restored.
7. Verify public route policy:
   ```bash
   curl -sS -i -X POST https://open-automation.org/devices \
     -H 'Content-Type: application/json' \
     -d '{"id":"route-smoke","name":"Route Smoke","type":"access_controller"}'
   curl -sS -i https://open-automation.org/devices/ | sed -n '1,12p'
   ```
   Expected: `POST /devices` returns `200`, while browser `GET /devices/`
   returns `401 Basic realm="Device Manager"` without credentials.
8. Verify local Device Manager and access-controller state:
   ```bash
   curl -sf http://192.168.1.40:8102/api/health
   curl -sf 'http://192.168.1.40:8102/api/devices/<device-manager-id>/access-controller/state' \
     | jq '{uuid:.state.device.uuid, ssid:.state.wifi.active_ssid, ip:.state.device.network.wifi_sta_ip, quality:.state.device.network.wifi_sta_quality, ota:.state.system.firmware.otaState}'
   ```
   If Device Manager hangs while health is stale or partial, restart it on
   Sonic: `ssh sonic 'docker restart device-manager'`.
9. Validate OTA through the user-facing path under test. For Device Manager OTA:
   ```bash
   curl -sS --max-time 180 -X POST \
     "http://192.168.1.40:8102/api/devices/<device-manager-id>/access-controller/ota" \
     -H 'Content-Type: application/octet-stream' \
     -H 'X-Firmware-Filename: controller.bin' \
     --data-binary @/home/andy/projects/access-controller/code/controller/build/controller.bin
   ```
   Confirm the running partition flips and `otaState` becomes `valid`.
10. Run the full automated suite:
    ```bash
    cd ~/projects/access-controller/code/controller/tests
    DEVICE_URL=http://<controller-ip> npm test
    ```
11. For UI/network changes, run a Playwright desktop/mobile smoke and inspect
    screenshots. The active Wi-Fi card must show SSID, quality/RSSI, connected
    age, STA IP, gateway, STA MAC, AP BSSID, channel, and security.

Normal successful testing must be quiet. Keep controller quiet-test mode enabled
for synthetic load/config traffic; beep only for unexpected failures or reboot
alerts.

## One-Hour Soak With OTA

Run the full load test with browser refreshes, settings churn, API traffic, uptime tick checks, and repeated OTA uploads:

```bash
cd ~/projects/access-controller/code/controller/tests
DEVICE_URL=http://192.168.1.131 \
SOAK_DURATION_MS=3600000 \
SOAK_STATE_WORKERS=2 \
SOAK_SETTINGS_WORKERS=1 \
SOAK_BROWSER_WORKERS=1 \
SOAK_BROWSER_REFRESH_MS=3000 \
SOAK_OTA_REPEATS=4 \
SOAK_AUDIBLE_ALERT=1 \
SOAK_PROGRESS_MS=60000 \
npm run test:soak
```

Expected evidence:
- `GET /api/state`, `/api/signals`, logs, Wi-Fi, Wiegand, RF, and discovery stay responsive.
- Settings POSTs for exit, FOB, keypad, and motion round-trip under load.
- Browser reloads succeed and System uptime text ticks every second.
- OTA upload/reboot succeeds for every scheduled upload.
- No unexpected reboot beyond scheduled OTA reboot events.
- Normal load testing is quiet: the suite enables controller quiet-test mode and sends synthetic settings updates with `alert:false`.
- If a non-OTA request failure or unexpected reboot is detected, the suite emits an audible terminal bell and attempts a forced controller error beep.
- Markdown and JSON reports are written to `code/controller/tests/artifacts/`.

The 4 state / 2 settings / 2 browser worker profile is a destructive stress profile for the current ESP32 build. Use it only when intentionally investigating overload behavior; it can wedge Wi-Fi/HTTP without a firmware panic. The acceptance soak is the 2/1/1 profile above, which still exercises continuous API traffic, browser refreshes, settings churn, uptime ticks, and OTA.

## Fast Smoke After Fixes

```bash
cd ~/projects/access-controller/code/controller/tests
DEVICE_URL=http://192.168.1.131 npm run test:quick
DEVICE_URL=http://192.168.1.131 SOAK_DURATION_MS=120000 SOAK_OTA_REPEATS=0 SOAK_AUDIBLE_ALERT=1 npm run test:soak
DEVICE_URL=http://192.168.1.131 npm run test:ui
```

## OTA Repetition Only

Use this when validating app-slot stability:

```bash
cd ~/projects/access-controller/code/controller
for i in 1 2 3 4 5; do
  python3 tools/ota_client.py --host http://192.168.1.131 --binary build/controller.bin --yes
  sleep 12
  curl -sf http://192.168.1.131/api/state | jq '{version:.system.firmware.projectVersion, ota:.system.firmware.otaState, running:.system.firmware.runningPartition.label, uptime:.system.uptimeSeconds}'
done
```

## Device Manager And Public `/devices`

The production path is:

- ESP32 posts/punches to `https://open-automation.org/devices`.
- Cloudflare/nginx forwards only punch/health traffic through the Sonic reverse SSH tunnel.
- Device Manager on `sonic` controls and OTAs the controller over LAN at the controller STA IP.
- The ESP32-side experimental reverse tunnel client is disabled by default with `CONFIG_ACCESS_CONTROLLER_ENABLE_TUNNEL=n`; do not re-enable it for normal testing because it consumes heap/socket capacity and the current implementation is not suitable for full OTA-sized bodies.

Verify the public punch path without exposing the full UI:

```bash
curl -sf -X POST https://open-automation.org/devices \
  -H 'Content-Type: application/json' \
  -d '{"uuid":"smoke-test","name":"codex-smoke","type":"access_controller"}'
curl -sS -i https://open-automation.org/devices/ | sed -n '1,12p'
```

Expected: the POST returns `200`, and unauthenticated browser GET returns
`401 Basic realm="Device Manager"`. Remove any temporary punch record after the
test from `sonic:/home/andy/projects/device-manager/data/discovery/punched.json`
using the Device Manager data-volume owner.

Verify Device Manager LAN-backed state:

```bash
ssh sonic 'curl -sf http://127.0.0.1:8102/api/devices/<device-manager-id>/access-controller/state \
  | jq "{ok, device_id, uuid:.state.device.uuid, ip:.state.device.network.wifi_sta_ip, running:.state.system.firmware.runningPartition.label, ota:.state.system.firmware.otaState}"'
```

Verify Device Manager OTA:

```bash
scp ~/projects/access-controller/code/controller/build/controller.bin sonic:/tmp/access-controller-candidate.bin
ssh sonic 'curl -sf --max-time 180 -X POST \
  http://127.0.0.1:8102/api/devices/<device-manager-id>/access-controller/ota \
  -H "Content-Type: application/octet-stream" \
  -H "X-Firmware-Filename: controller.bin" \
  --data-binary @/tmp/access-controller-candidate.bin | jq .'
```

After Device Manager OTA, confirm both direct LAN and Device Manager state return `otaState: valid`.

## Erase-Flash And AP Recovery

This is destructive to saved settings. Preserve the known Wi-Fi credentials first.

1. Build the app:
   ```bash
   cd ~/projects/access-controller/code/controller
   source ~/esp/esp-idf/export.sh
   idf.py build
   ```

2. Erase and flash over serial:
   ```bash
   ESP_PORT=/dev/ttyUSB0 idf.py -p /dev/ttyUSB0 erase-flash
   ./flash_now.sh
   ```

3. Verify AP mode:
   - Connect the host Wi-Fi to the controller AP.
   - Open or query `http://192.168.4.1/api/state`.
   - Confirm `device.network.wifi_ap_ip` is `192.168.4.1` and STA IP is empty.
   - Confirm `device.uuid` is stable for this board across full-chip erases.

4. Restore Wi-Fi from AP mode:
   ```bash
   curl -sf -X POST http://192.168.4.1/api/wifi/add \
     -H 'Content-Type: application/json' \
     -d '{"ssid":"HelloWorld","password":"qscwdvpk"}'
   ```

5. Wait for STA mode:
   ```bash
   for i in $(seq 1 90); do
     curl -sf http://192.168.1.131/api/state && break
     sleep 2
   done
   ```

6. Confirm server policy and tunnel target:
   ```bash
   curl -sf http://192.168.1.131/api/state | jq '{server:.server, network:.device.network, firmware:.system.firmware}'
   ```

## AP Recovery From Bad Wi-Fi

To test fallback without erasing flash:

1. From STA mode, save an intentionally bad Wi-Fi network via the settings UI or API.
2. Reboot or power-cycle the controller.
3. Confirm the device starts AP mode when STA cannot connect.
4. Leave it for at least one recovery interval. The firmware should retry saved credentials periodically and switch to STA mode when a valid network is restored.
5. During AP mode, run a short soak against `http://192.168.4.1`:
   ```bash
   cd ~/projects/access-controller/code/controller/tests
   DEVICE_URL=http://192.168.4.1 SOAK_DURATION_MS=300000 SOAK_OTA_REPEATS=0 SOAK_AUDIBLE_ALERT=1 npm run test:soak
   ```

## Pass Criteria

- No non-OTA request failures in a one-hour soak.
- p95 state and signal latency stays below 1000 ms on LAN.
- Browser refresh p95 stays below 3000 ms on LAN.
- Uptime text changes every second in the UI.
- No normal beeps during passing load traffic; audible alerts occur only for failure/reboot conditions.
- Every scheduled OTA returns online, lands in the alternate app slot, and becomes `valid`.
- Device Manager can fetch state and proxy OTA over LAN from `sonic`; public `POST /devices` accepts punch-in, and public browser `GET /devices/` is authenticated.
- Active Wi-Fi metrics are visible through direct controller state, Device Manager state, and the Settings UI: quality/RSSI, STA IP, gateway, STA MAC, AP BSSID, channel, and security.
- Two saved networks can be switched both directions when available, with active SSID/IP/link metrics changing to match the current AP.
- Flash erase produces the same MAC-derived UUID, enters AP mode, and Wi-Fi reprovisioning returns to STA mode.
- Reports and any fixes are committed with the exact verification commands in the final summary.
