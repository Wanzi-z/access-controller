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

## One-Hour Soak With OTA

Run the full load test with browser refreshes, settings churn, API traffic, uptime tick checks, and repeated OTA uploads:

```bash
cd ~/projects/access-controller/code/controller/tests
DEVICE_URL=http://192.168.1.131 \
SOAK_DURATION_MS=3600000 \
SOAK_STATE_WORKERS=4 \
SOAK_SETTINGS_WORKERS=2 \
SOAK_BROWSER_WORKERS=2 \
SOAK_BROWSER_REFRESH_MS=3000 \
SOAK_OTA_REPEATS=4 \
SOAK_PROGRESS_MS=60000 \
npm run test:soak
```

Expected evidence:
- `GET /api/state`, `/api/signals`, logs, Wi-Fi, Wiegand, RF, and discovery stay responsive.
- Settings POSTs for exit, FOB, keypad, and motion round-trip under load.
- Browser reloads succeed and System uptime text ticks every second.
- OTA upload/reboot succeeds for every scheduled upload.
- No unexpected reboot beyond scheduled OTA reboot events.
- Markdown and JSON reports are written to `code/controller/tests/artifacts/`.

## Fast Smoke After Fixes

```bash
cd ~/projects/access-controller/code/controller/tests
DEVICE_URL=http://192.168.1.131 npm run test:quick
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
   DEVICE_URL=http://192.168.4.1 SOAK_DURATION_MS=300000 SOAK_OTA_REPEATS=0 npm run test:soak
   ```

## Pass Criteria

- No non-OTA request failures in a one-hour soak.
- p95 state and signal latency stays below 1000 ms on LAN.
- Browser refresh p95 stays below 3000 ms on LAN.
- Uptime text changes every second in the UI.
- Every scheduled OTA returns online, lands in the alternate app slot, and becomes `valid`.
- Flash erase produces AP mode and Wi-Fi reprovisioning returns to STA mode.
- Reports and any fixes are committed with the exact verification commands in the final summary.
