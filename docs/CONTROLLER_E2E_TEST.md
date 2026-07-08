# Controller Factory E2E Test Procedure

This runbook is for validating a controller from a factory-like flash through
Wi-Fi provisioning, tunnel visibility, and real hardware input/output checks.

## Test Scope

The test is passing only when all of these are confirmed with live evidence:

- Full flash erase was performed before flashing the active firmware.
- The freshly flashed controller boots without saved Wi-Fi credentials and
  starts its AP hotspot.
- The controller can be provisioned onto the target Wi-Fi network.
- The controller is reachable through its local station IP after provisioning.
- The controller can reach the tunnel server and is visible through the server
  route used by `https://open-automation.org/devices`.
- Real hardware signals change as expected for both controller channels:
  lock output, lock contact/sense, exit button, keypad/fob/motion inputs.
- Live Wiegand RFID data is captured by registration mode.
- Live 433 MHz RF remote data is captured by RF registration mode.
- Automated API/UI tests and the interactive physical hardware suite pass.

## Environment

- Repo: `/home/andy/projects/access-controller`
- Active firmware: `code/controller`
- USB serial port used in this run: `/dev/ttyUSB0`
- ESP-IDF observed in this run: `v6.0-dev-1002-gbfe5caf58f`
- esptool observed in this run: `v4.9.0`
- AP fallback URL: `http://192.168.4.1/`
- AP password: `pyfitech`
- Existing LAN device URL before factory reset: `http://192.168.1.131/`
- Server URL under test: `https://open-automation.org/devices`
- Device-manager UI, not public E2E target: `http://192.168.1.40:8102/`
- Server host requested by user for validation: `ssh sonic`

## Live Evidence Log

Record every run in this section. Use exact timestamps and paste short command
outputs or report filenames instead of prose-only claims.

### 2026-07-08 New Controller Deploy + Echo42/Device Manager Validation

The complete current procedure is captured in
`docs/CONTROLLER_DEPLOY_AND_TEST.md`. This log records the concrete known-good
evidence from the July 8 run.

- Programmed ESP32-S3 board over `/dev/ttyUSB0`.
- Board MAC: `b8:f8:62:cb:9a:a0`.
- Controller UUID: `acce5501-b8f8-42cb-9aa0-05c0085e08b7`.
- First AP observed after flash: `ac_08b7`, password `pyfitech`,
  `192.168.4.1`.
- Provisioned from AP mode onto `Echo42`; controller STA IP:
  `10.69.136.23`.
- Controller server URL: `https://open-automation.org/devices`.
- Firmware OTA uploaded via direct controller API:
  `POST http://10.69.136.23/api/ota/upload`, response included
  `{"ok":true,"reboot":true}`, partition `app1`.
- Post-OTA direct state returned:
  - UUID `acce5501-b8f8-42cb-9aa0-05c0085e08b7`
  - active SSID `Echo42`
  - STA IP `10.69.136.23`
  - gateway `10.69.136.163`
  - `wifi_sta_quality`, `wifi_sta_rssi`, channel, auth, and AP BSSID present
  - running partition `app1`
  - OTA state `valid`
- Device Manager was restored with `docker restart device-manager` on `sonic`
  after `/` and `/devices` hung while `/api/health` still returned 200.
- Device Manager local health:
  `curl http://192.168.1.40:8102/api/health` returned
  `{"ok":true,"service":"device-manager","version":"0.1.0"}`.
- Device Manager ID used for this controller:
  `61c08cc8-5225-545e-b987-5a0be207b871`.
- Device Manager access-controller state route returned matching UUID, Echo42
  SSID, `10.69.136.23`, Wi-Fi quality, and OTA `valid`.
- Public route behavior:
  - `POST https://open-automation.org/devices` returned `200
    application/json`.
  - `GET https://open-automation.org/devices/` returned `401 Basic
    realm="Device Manager"`, confirming the public UI is authenticated rather
    than open.
- UI Settings page was verified with Playwright at desktop and mobile widths.
  The Active Network card rendered link quality/RSSI, connected age, STA IP,
  gateway, STA MAC, AP BSSID, channel, and security.
- Targeted tests after OTA:
  - `DEVICE_URL=http://10.69.136.23 npm run test:api`: `77 passed, 0 failed`.
  - `DEVICE_URL=http://10.69.136.23 npm run test:ui`: `51 passed, 0 failed,
    2 skipped`.
- Full automated suite after OTA:
  - `DEVICE_URL=http://10.69.136.23 npm test`: `161 passed, 0 failed,
    2 skipped`.
- Final state after the full suite had no test PIN users and no test RF remotes
  left behind.

### 2026-07-06 Factory E2E Run

Pre-reset observations:

- `/dev/ttyUSB0` existed and was not held open by another process.
- `idf.py --version`: `ESP-IDF v6.0-dev-1002-gbfe5caf58f`
- `python -m esptool version`: `esptool.py v4.9.0`
- `GET http://192.168.1.131/api/discovery` passed.
- Device UUID before reset: `2d97094e-253e-4155-b8ad-d22246644d03`
- Station IP before reset: `192.168.1.131`
- Wi-Fi before reset: `HelloWorld`
- Firmware before reset:
  - project version: `f7d33f1`
  - build date/time: `Jul 6 2026 20:18:53`
  - running partition: `app0`
  - OTA state: `valid`
- `ssh sonic` succeeded and showed listeners on `9000`, `9001`, `80`, and
  `443`.
- `curl https://open-automation.org/devices` returned `403 Forbidden` from
  nginx/Cloudflare.
- `ssh sonic 'curl http://127.0.0.1:9000/devices'` returned `403 Forbidden`
  from MinIO `AccessDenied`, so the public `/devices` route is not yet proven
  as a working tunnel UI route in this run.

Server route correction:

- `device-manager-tunnel.service` on `sonic` forwards the local device-manager
  UI/backend from `192.168.1.40:8102` to the public host on port `3035`.
- Public nginx on `open-automation.org` is intentionally configured to block
  the dashboard and expose only device/API routes.
- Added exact nginx route `location = /devices` on the public host to proxy to
  `http://127.0.0.1:3035/api/devices/punch`.
- Verified `nginx -t` passed and nginx reloaded.
- Verified `POST https://open-automation.org/devices` returns `200` with:
  `{"ok":true,"device_id":"e2e-route-test",...}`.
- Verified a second manual route smoke using the freshly flashed controller UUID
  `b9a722a9-a49a-4431-80cf-4712fc1a1f7e` returned `200` and backend logs on
  `sonic` showed `POST /api/devices/punch HTTP/1.1" 200 OK`.
- Verified `GET https://open-automation.org/` remains `403`, so the UI is not
  exposed publicly at the root.
- Verified public TLS certificate is valid for `open-automation.org`; issuer is
  `Google Trust Services WE1`, valid from `2026-07-07` to `2026-10-05`.

Firmware route correction:

- Added station-mode boot punch from the controller firmware to the configured
  server URL.
- The default configured server URL is `https://open-automation.org/devices`.
- HTTPS uses the ESP-IDF certificate bundle (`esp_crt_bundle_attach`) so normal
  publicly signed server certificates are trusted without pinning a single cert.

Factory erase/flash evidence after firmware route correction:

- Full chip erase completed successfully on ESP32-S3 MAC `b8:f8:62:cb:9a:b0`.
- Build completed; `controller.bin` size: `0x139c40`, with `0xc63c0` bytes
  free in the smallest app partition.
- Flash completed and all esptool hashes verified.
- First boot after erase:
  - Device UUID: `b9a722a9-a49a-4431-80cf-4712fc1a1f7e`
  - Wi-Fi credentials: empty SSID/password
  - AP SSID: `ac_1f7e`
  - AP password: `pyfitech`
  - AP IP/DHCP: `192.168.4.1`
  - Wiegand channel 1/2 initialized on GPIO4/5 and GPIO6/7
  - RF receiver initialized on GPIO15
  - MCP23017 communication successful
  - SPIFFS formatted after erase, then mounted successfully

AP-mode validation and debounce fix:

- Office machine Wi-Fi was soft-blocked; enabled Wi-Fi and scanned the AP.
- Verified AP `ac_1f7e` visible with BSSID `B8:F8:62:CB:9A:B1` and strong
  signal.
- Connected to `ac_1f7e`; office machine received `192.168.4.2`.
- `GET http://192.168.4.1/api/discovery` passed for UUID
  `b9a722a9-a49a-4431-80cf-4712fc1a1f7e`.
- `DEVICE_URL=http://192.168.4.1 npm run test:quick`: `37 passed, 0 failed`.
- First `npm run test:api` exposed a firmware bug: CH1 lock
  `enableContactAlert` did not toggle because lock-message debounce applied to
  all config writes.
- Fixed debounce so it applies only to rapid arm/disarm actions, not config
  fields.
- Added `RF Remote Registration Capture` to
  `code/controller/tests/suites/physical-hardware.mjs`.
- Added `npm run test:server-route`, a repeatable public route smoke that
  verifies `/api/health`, `POST /devices`, blocked `/`, and non-HTML
  `GET /devices` behavior.
- Rebuilt firmware successfully after the debounce and RF-test changes.

Factory erase/flash evidence after debounce fix:

- Full chip erase completed successfully again.
- Flash completed and all esptool hashes verified again.
- First boot after the final erase:
  - Device UUID: `650af534-cc7f-43b2-8dd4-0c5501c8688f`
  - AP SSID: `ac_688f`
  - AP password: `pyfitech`
  - AP IP/DHCP: `192.168.4.1`
  - Firmware ELF SHA prefix: `66e872adb`
  - Wi-Fi station IP: `null`
  - Server URL: `https://open-automation.org/devices`
- Connected office machine to AP `ac_688f`; office machine reached
  `http://192.168.4.1/api/discovery`.
- `DEVICE_URL=http://192.168.4.1 npm run test:quick`: `37 passed, 0 failed`.
- `DEVICE_URL=http://192.168.4.1 npm run test:api`: `66 passed, 0 failed`.
- `DEVICE_URL=http://192.168.4.1 npm run test:ui`: `43 passed, 0 failed,
  2 skipped` (`Keypad user add` and `Wiegand section` were skipped by the
  existing UI suite).
- `npm run test:server-route`: `4 passed, 0 failed`.
- Rechecked current live state after the final flash: controller remains in AP
  mode with `wifi_sta_ip: null`, AP `ac_688f` is visible, and no controller-side
  punch for UUID `650af534-cc7f-43b2-8dd4-0c5501c8688f` has appeared yet.
- Later recheck at `2026-07-06T21:03:31-05:00`:
  - `/dev/ttyUSB*` and `/dev/ttyACM*` were absent.
  - `lsusb` did not show the controller USB serial device.
  - Wi-Fi scan no longer showed AP `ac_688f`.
  - LAN scan did not find an access-controller `/api/discovery` response.
  - `sonic` device-manager punched inventory showed manual route-smoke records,
    but no current UUID `650af534-cc7f-43b2-8dd4-0c5501c8688f` controller punch.
  - Conclusion: the controller is not currently visible from this office machine
    and is not yet proven visible through `https://open-automation.org/devices`.
- Rechecked again at `2026-07-06T21:04:42-05:00`:
  - No `/dev/ttyUSB*` or `/dev/ttyACM*` controller serial node.
  - Wi-Fi scan did not show AP `ac_688f`.
  - `curl http://192.168.4.1/api/discovery` timed out.
  - LAN scan across `192.168.1.1-254` found no access-controller discovery
    response.
  - Recent `sonic` device-manager logs showed route-smoke `POST
    /api/devices/punch` entries, but no current UUID
    `650af534-cc7f-43b2-8dd4-0c5501c8688f` evidence.
- Rechecked again at `2026-07-06T21:07:28-05:00`:
  - `GET https://open-automation.org/` returned `403`, confirming the old
    public dashboard is still blocked.
  - `GET https://open-automation.org/devices` returned `404
    application/json`, confirming the dashboard HTML is not exposed there.
  - `POST https://open-automation.org/devices` returned `200
    application/json` for manual route smoke `route-recheck-latest`.
  - `npm run test:server-route`: `4 passed, 0 failed`.
  - `ssh sonic` showed `device-manager-tunnel.service` active and the
    `device-manager` container healthy on port `8102`.
  - No controller USB serial node was present; `lsusb` showed only the
    unrelated FTDI dual-UART adapter.
  - Wi-Fi scan did not show AP `ac_688f`.
  - `curl http://192.168.4.1/api/discovery` timed out.
  - LAN scan did not find a reachable access-controller discovery endpoint.
  - Conclusion: public route policy is verified, but the physical controller is
    still not visible from this machine, so the controller-originated station
    punch remains unverified.
- Rechecked and hardened public tunnel exposure at `2026-07-06T21:15:36-05:00`:
  - Found the active device-manager checkout on `sonic` at
    `/home/andy/projects/device-manager`; the requested
    `~/project/device-manager` path does not exist.
  - Confirmed `device-manager-tunnel.service` forwards
    `127.0.0.1:3035` on the public host to `localhost:8102` on `sonic`.
  - Confirmed the public nginx config for `open-automation.org` blocks `/`,
    proxies `/api/devices/punch` and `/api/health`, and aliases exact
    `/devices` to `/api/devices/punch`.
  - Found an exposure bypass: the public host sshd had global
    `GatewayPorts yes`, causing reverse tunnel port `3035` to bind
    `0.0.0.0`; `curl http://142.93.57.114:3035/` returned the full
    device-manager dashboard.
  - Changed public host sshd global config to `GatewayPorts clientspecified`,
    leaving the existing `Match User tunnel` override intact.
  - Restarted `device-manager-tunnel.service`; public host now shows
    `127.0.0.1:3035` only.
  - Restarted other `sonic`-managed reverse tunnels so ports `3030-3033` also
    rebound to `127.0.0.1`.
  - Killed/reconnected the stale ssh tunnel that had exposed `3034` and `2234`;
    it also rebound to loopback under the new sshd policy.
  - External direct-port checks for `2234` and `3030-3035` now fail from this
    machine.
  - `GET https://open-automation.org/` still returns `403`.
  - `POST https://open-automation.org/devices` still returns `200`.
  - `npm run test:server-route`: `4 passed, 0 failed`.
  - Controller USB came back as `/dev/ttyUSB0` and AP `ac_688f` is visible.
  - `GET http://192.168.4.1/api/discovery` passed for UUID
    `650af534-cc7f-43b2-8dd4-0c5501c8688f`.
  - `DEVICE_URL=http://192.168.4.1 npm run test:quick`: `37 passed,
    0 failed`.
  - RF registration API was tested twice after the RF receiver was connected.
    Registration mode started, but no fob was captured:
    `users: []`, `registrationPending: 0`, `lastDuplicateCode: ""`.
  - Opening serial for RF logs reset the board, so that serial capture is not
    treated as valid RF evidence.
- RF retry at `2026-07-06T21:17:00-05:00`:
  - Ran a 45 second RF registration window without opening serial.
  - Operator was instructed to press RF fob buttons repeatedly during the
    window.
  - Stop/final RF state remained:
    `{"registrationActive":false,"registrationPending":0,"lastDuplicateCode":"","users":[]}`.
  - Conclusion: RF registration endpoint works, but no valid RF remote code has
    reached the registry yet. This is still a failing/unproven physical RF
    check.
- RF receiver investigation at `2026-07-06T21:23:43-05:00`:
  - Traced the PCB/netlist for RF input:
    - RXB6 module is `J22`.
    - `J22` pin 2 is RF `DATA`.
    - `DATA` drives Q4 base through `R83` (`10K`).
    - Q4 is an NPN `MMBT5551`; emitter is ground.
    - Q4 collector is `DATA_IO`, pulled up by `R84` (`10K`) and connected to
      ESP32-S3 `U7` pin 8, `GPIO15/U0RTS/ADC2_CH4/XTAL_32K_P`.
    - Therefore the RF module signal is inverted at the ESP32 input: RF module
      high becomes GPIO15 low.
  - Firmware had been listening on the correct ESP32 IO (`GPIO15`), but the
    old decoder expected a long high sync at GPIO15. Added RF receiver
    diagnostics and changed the decoder to treat the board signal as inverted.
  - Serial bootloader flashing failed because the chip was not entering download
    mode; deployed the diagnostic firmware through `POST /api/ota/upload`
    instead.
  - OTA response:
    `{"ok":true,"reboot":true,"bytes":1286256,"partition":"app1",...}`.
  - Diagnostic firmware booted on `app1`, ELF SHA prefix `c35277561`.
  - Ran a 30 second `Add Credential` enrollment window while operator pressed
    RF fobs.
  - RF diagnostics after the fob window:
    `edgeCount=172799`, `noiseCount=61722`, `syncCount=0`,
    `captureCount=0`, `decodeOkCount=0`, `decodeFailCount=0`,
    `lastPulseUs=161`, `users=[]`, `remoteCount=0`.
  - `DEVICE_URL=http://192.168.4.1 npm run test:quick`: `37 passed,
    0 failed`.
  - Conclusion: UI enrollment and RF API are listening, and GPIO15 is changing,
    but no valid RF sync pulse is reaching the decoder. Remaining suspects are
    RF module wiring/power/antenna, RF fob frequency/protocol, or signal
    conditioning/timing outside the current RXB6 EV1527/PT2262 assumptions.

Next evidence to collect:

- Optional phone screenshot confirming AP `ac_688f` is visible; office machine
  scan and connection already confirmed it.
- Provisioning request/response for the phone hotspot.
- Post-provision station IP and `/api/discovery` output.
- Server punch evidence showing UUID `650af534-cc7f-43b2-8dd4-0c5501c8688f`
  posted by the controller firmware to `https://open-automation.org/devices`
  after station-mode connect. The manual route smoke proves the server route,
  but not the controller's Wi-Fi/client path.
- Re-run `npm run test:quick`, `npm run test:api`, and `npm run test:ui` after
  station-mode provisioning.
- `npm run test:physical` report with no skipped physical checks, including the
  new RF registration capture.

## Factory Erase And Flash

Use the board-specific CP2102N DTR/RTS sequence from `flash_now.sh`; it has been
verified for this controller hardware.

From the active firmware directory:

```bash
cd /home/andy/projects/access-controller/code/controller
source /home/andy/esp/esp-idf/export.sh
```

Confirm the port:

```bash
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true
fuser -v /dev/ttyUSB0 2>/dev/null || true
```

Enter download mode and erase the whole flash:

```bash
python - /dev/ttyUSB0 <<'PY'
import serial, sys, time
port = sys.argv[1]
ser = serial.Serial(port, 115200, timeout=0.1, dsrdtr=False, rtscts=False)
ser.dtr = True
ser.rts = True
time.sleep(0.1)
ser.dtr = False
ser.rts = False
time.sleep(0.25)
ser.rts = True
time.sleep(0.5)
ser.close()
PY

python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 \
  --before no_reset --after no_reset erase_flash
```

Build and flash:

```bash
ESP_PORT=/dev/ttyUSB0 ./flash_now.sh
```

Monitor first boot:

```bash
idf.py -p /dev/ttyUSB0 monitor
```

Expected first-boot evidence:

- NVS starts empty or is re-initialized.
- A deterministic MAC-derived identity is printed as `Device UUID: ...`; repeated full-chip erases of the same board must produce the same UUID.
- Wi-Fi station mode fails due to no saved valid Wi-Fi credentials.
- AP mode starts with an `ac_...` SSID and password `pyfitech`.
- Device UI is reachable at `http://192.168.4.1/` after joining the AP.

## Wi-Fi Provisioning

1. Join the controller AP from a phone/laptop.
2. Open `http://192.168.4.1/`.
3. Add the target phone hotspot credentials.
4. Wait for the controller to restart.
5. Confirm the controller joins the hotspot and obtain the station IP.

API equivalent while connected to the controller AP:

```bash
curl -sS -X POST http://192.168.4.1/api/wifi/add \
  -H 'Content-Type: application/json' \
  -d '{"ssid":"<PHONE_HOTSPOT_SSID>","password":"<PHONE_HOTSPOT_PASSWORD>"}'
```

After restart, verify:

```bash
curl -sS http://<STATION_IP>/api/discovery | jq .
curl -sS http://<STATION_IP>/api/state | jq '{device, wifi, server}'
```

## Server Verification

The public UI must not be exposed through `open-automation.org`. The allowed
device entrypoint is:

```text
POST https://open-automation.org/devices
```

Public route smoke:

```bash
cd /home/andy/projects/access-controller/code/controller/tests
npm run test:server-route
```

Manual equivalent:

```bash
curl -sS -i https://open-automation.org/api/health
curl -sS -i -X POST https://open-automation.org/devices \
  -H 'Content-Type: application/json' \
  -d '{"id":"route-smoke","name":"Route Smoke","type":"access_controller"}'
curl -sS -i https://open-automation.org/
```

Expected:

- `/api/health` returns `200`.
- `POST /devices` returns `200`.
- `/` returns `403`.

On `sonic`, check the device-manager backend and tunnel:

```bash
ssh sonic 'systemctl status device-manager-tunnel.service --no-pager -l'
ssh sonic 'docker logs --since 10m device-manager | grep "/api/devices/punch"'
```

On the public host:

```bash
ssh andy@142.93.57.114 'sudo nginx -t'
ssh andy@142.93.57.114 'sudo tail -n 50 /var/log/nginx/access.log | grep devices'
```

For live controller pass evidence, the access log and backend log must show a
`POST /devices` or `POST /api/devices/punch` for the controller's current UUID
after it joins station Wi-Fi.

Legacy TCP tunnel checks, if still used by the firmware/server:

```bash
ssh sonic 'ss -ltnp | grep -E ":(9000|9001)\b"'
ssh sonic 'journalctl -u device-manager-tunnel.service --since "10 minutes ago" --no-pager'
```

## Automated Tests

From the test directory:

```bash
cd /home/andy/projects/access-controller/code/controller/tests
DEVICE_URL=http://<DEVICE_IP> npm run test:quick
DEVICE_URL=http://<DEVICE_IP> npm run test:api
DEVICE_URL=http://<DEVICE_IP> npm run test:ui
```

The report is written to `code/controller/tests/test-report.html`.

## Physical Hardware Tests

Run the interactive suite with the post-provision URL:

```bash
cd /home/andy/projects/access-controller/code/controller/tests
DEVICE_URL=http://<DEVICE_IP> npm run test:physical
```

Do not skip any item for a full pass. The operator must trigger each item when
prompted and the suite must observe the matching state/log change:

- CH1 exit button
- CH2 exit button
- CH1 lock contact/sense transition
- CH2 lock contact/sense transition
- CH1 keypad/PIN event
- CH2 keypad/PIN event
- CH1 registered fob event
- CH2 registered fob event
- Wiegand RFID registration capture
- Motion input event
- Buzzer/sounder confirmation

RF remote registration is also required for this E2E scope. If the current
interactive physical suite does not prompt for a raw RF registration capture,
run it manually:

```bash
curl -sS -X POST http://<DEVICE_IP>/api/rf/register \
  -H 'Content-Type: application/json' -d '{}'
# press the RF remote now
curl -sS -X POST http://<DEVICE_IP>/api/rf/stop \
  -H 'Content-Type: application/json' -d '{}' | jq .
```

Passing evidence is a non-empty RF user/code in the stop response or in:

```bash
curl -sS http://<DEVICE_IP>/api/rf | jq .
```

Clean up test RF records after evidence is captured:

```bash
curl -sS -X POST http://<DEVICE_IP>/api/rf/delete-all \
  -H 'Content-Type: application/json' -d '{}'
```

## Pass/Fail Checklist

- [ ] Full flash erase completed.
- [ ] Firmware build completed.
- [ ] Firmware flash completed.
- [ ] First boot entered AP mode.
- [ ] AP hotspot visible externally.
- [ ] AP UI/API reachable at `192.168.4.1`.
- [ ] Phone hotspot credentials saved.
- [ ] Controller joined the phone hotspot.
- [ ] Local station API reachable.
- [ ] Server route/tunnel visibility confirmed from `sonic`.
- [ ] Quick/API/UI automated tests passed.
- [ ] CH1 and CH2 lock/contact/exit/keypad/fob/motion paths passed.
- [ ] Wiegand RFID live capture passed.
- [ ] RF remote live capture passed.
- [ ] No physical test was skipped.
