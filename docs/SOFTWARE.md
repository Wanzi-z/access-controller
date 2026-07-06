# Software

## Active Firmware: `code/controller`

`code/controller` is the active ESP32-S3 firmware. It is an ESP-IDF CMake
project named `controller`.

Key files:

- `code/controller/CMakeLists.txt`: root ESP-IDF project configuration.
- `code/controller/main/main.c`: boot sequence and service startup.
- `code/controller/main/CMakeLists.txt`: component registration and embedded UI
  asset list.
- `code/controller/main/Kconfig.projbuild`: firmware configuration options.
- `code/controller/partitions.csv`: NVS, OTA, app, and SPIFFS partition layout.
- `code/controller/main/public`: browser UI embedded into firmware.

The root CMake file currently sets:

```cmake
set(EXTRA_COMPONENT_DIRS "/home/andy/esp/esp-protocols/components/esp_websocket_client")
project(controller)
```

That means the build environment expects ESP-IDF and an `esp_websocket_client`
component at that local path.

### Boot Flow

`main.c` includes service `.c` files directly and calls service initializers in
this order after network/AP selection:

1. `gpio_main`
2. `i2c_main`
3. `mcp23x17_main`
4. `auth_main`
5. `buzzer_main`
6. `wiegand_registry_init`
7. `wiegand_main`
8. `exit_main`
9. `motion_main`
10. `keypad_main`
11. `fob_main`
12. `rf_registry_init`
13. `rf_receiver_init`
14. `lock_main`
15. `server_main`

After services start, the firmware initializes SPIFFS, sends user count, and
logs periodic heap/NVS/uptime status once per minute.

### Core Services

| Service file | Purpose |
| --- | --- |
| `automation.c/.h` | Global device ID/token/server variables, service/client queues, persistent log integration, boot/reset logs, lock action source tracking. |
| `station.c` | Wi-Fi station connection and saved network helpers. |
| `ap.c` | AP fallback mode. The generated AP SSID is based on the device UUID suffix and uses password `pyfitech`. |
| `gpio.c` | Direct ESP32 GPIO definitions and wrapper functions that route I/O through MCP23017 when enabled. |
| `drivers/i2c.c` | I2C master on GPIO13 SCL and GPIO14 SDA. |
| `drivers/mcp23x17.c/.h` | MCP23017 I/O expander at address `0x20`, polled every service loop. |
| `lock.c` | Two lock channels, control outputs, contact/sense inputs, polarity, alerts, persisted arm/enable state. |
| `exit.c` | Two exit button inputs and auto re-arm timers. |
| `keypad.c` | Two keypad button inputs and keypad re-arm settings. This is separate from Wiegand keypad PIN decoding. |
| `motion.c` | Two motion inputs and auto re-arm timers. |
| `fob.c` | Two physical fob inputs with momentary or latch behavior. |
| `wiegand.c/.h` | Two Wiegand channels, GPIO ISR capture, frame timeout parsing, card/PIN authorization, registration session state. |
| `wiegand_registry.c/.h` | In-memory Wiegand user registry persisted as JSON in NVS key `wiegand_users`. |
| `rf_receiver.c` | 433 MHz RXB6 pulse capture on GPIO15 and RF code decode. |
| `rf_registry.c/.h` | Learned RF remote registry persisted as JSON in NVS key `rf_fobs`. |
| `authorize.c` | PIN authorization, add-user behavior, Wi-Fi/server/device settings message handler. |
| `store.c/.h` | NVS string/bool/u32 helpers, SPIFFS mounting, settings persistence, keypad user JSON files. |
| `log_store.c/.h` | Persistent system log storage used by automation logs and `/api/logs`. |
| `server.c` | Starts the HTTP server and route registration. |
| `api.c` | JSON REST API endpoints and state snapshots. |
| `file_server.c` | Static file serving/upload/delete helpers. |
| `ws_server.c`, `ws_client.c` | WebSocket-related server/client code retained in the firmware tree. |
| `tunnel.c` | ESP32 tunnel client that speaks the Node gateway frame protocol. |
| `utilities_server.c` | Utility server functions. |
| `buzzer.c` | Keypad/buzzer feedback using MCP push pins. |

### Important I/O Mappings

These mappings are defined in source and should be kept in sync with hardware
docs when the board changes.

Direct ESP32 pins from `gpio.c`:

| Signal | GPIO |
| --- | --- |
| Wiegand channel 1 DATA0 | GPIO4 |
| Wiegand channel 1 DATA1 | GPIO5 |
| Wiegand channel 2 DATA0 | GPIO6 |
| Wiegand channel 2 DATA1 | GPIO7 |
| I2C SCL | GPIO13 |
| I2C SDA | GPIO14 |
| RF data input | GPIO15 |
| MCP INTA | GPIO34 |
| MCP INTB | GPIO35 |

MCP23017 pins used by active services:

| Signal | MCP pin |
| --- | --- |
| Lock channel 1 output | A0 |
| Lock channel 1 sense | A1 |
| Lock channel 1 contact/close | A2 |
| Keypad channel 1 button | A3 |
| Keypad channel 1 push/beep | A4 |
| Exit channel 1 button | A5 |
| Motion channel 1 | A6 |
| Fob channel 1 | A7 |
| Lock channel 2 output | B0 |
| Lock channel 2 sense | B1 |
| Lock channel 2 contact/close | B2 |
| Keypad channel 2 button | B3 |
| Keypad channel 2 push/beep | B4 |
| Exit channel 2 button | B5 |
| Motion channel 2 | B6 |
| Fob channel 2 | B7 |

### Storage Model

| Data | Storage |
| --- | --- |
| Device UUID | NVS string key `device_id` |
| Device token | NVS string key `token`, defaults to UUID when missing |
| Wi-Fi credentials/list | NVS via `station.c`/`store.c` helpers |
| Server/tunnel host and port | NVS strings including `server_ip`, `server_port`, `tunnel_host`, `tunnel_port` |
| Lock settings | NVS bool keys such as `lock_1_enable`, `lock_1_arm`, `lock_1_alert`, `lock_1_pol` |
| Wiegand users | NVS JSON string key `wiegand_users` |
| RF remotes | NVS JSON string key `rf_fobs` |
| PIN/keypad users | SPIFFS JSON files such as `/spiffs/user_00001.json`, indexed by NVS key `auth_user_count` |
| System logs | `log_store` persistent storage |

### HTTP API

Routes are registered in `code/controller/main/services/api.c`.

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/state` | Full state snapshot: device UUID, locks, exits, fobs, keypads, motions, Wiegand, RF, and Wi-Fi summary. |
| `GET` | `/api/logs` | Persistent system log snapshot. |
| `POST` | `/api/lock` | Update lock enable, arm, contact-alert, and polarity settings. |
| `POST` | `/api/exit` | Update exit button enable, alert, and delay settings. |
| `POST` | `/api/fob` | Update physical fob enable, alert, and latch settings. |
| `POST` | `/api/keypad` | Update keypad button enable, alert, and delay settings. |
| `POST` | `/api/motion` | Update motion enable, alert, and delay settings. |
| `GET` | `/api/keypad/users` | List PIN users. |
| `POST` | `/api/keypad/user` | Create PIN user with name and 4-8 digit PIN. |
| `PUT` | `/api/keypad/user` | Rename/update a PIN user. |
| `DELETE` | `/api/keypad/user` | Delete PIN user by UUID. |
| `GET` | `/api/wiegand` | Wiegand registration/users state. |
| `POST` | `/api/wiegand/register` | Start RFID/card registration for all channels or one channel. |
| `POST` | `/api/wiegand/stop` | Stop Wiegand registration and optionally promote pending users. |
| `POST` | `/api/wiegand/rename` | Rename a Wiegand user and promote it active on success. |
| `POST` | `/api/wiegand/delete` | Delete a Wiegand user by ID. |
| `GET` | `/api/rf` | RF remote registry state. |
| `POST` | `/api/rf/register` | Start RF remote registration. |
| `POST` | `/api/rf/stop` | Stop RF remote registration. |
| `POST` | `/api/rf/rename` | Rename an RF remote. |
| `POST` | `/api/rf/delete` | Delete an RF remote. |
| `POST` | `/api/rf/config` | Update RF mode, channel mask, exit seconds, and alert. |
| `GET` | `/api/wifi` | State snapshot with Wi-Fi information. |
| `POST` | `/api/wifi` | Legacy/general Wi-Fi settings handler through authorization message handling. |
| `GET` | `/api/wifi/list` | Saved Wi-Fi network list. |
| `POST` | `/api/wifi/add` | Add Wi-Fi credentials, then restart. |
| `POST` | `/api/wifi/delete` | Delete saved Wi-Fi network. |
| `POST` | `/api/wifi/connect` | Set active Wi-Fi network, then restart. |
| `POST` | `/api/server` | Update server IP/port through authorization message handling. |
| `GET` | `/favicon.ico` | Empty favicon response. |

### Embedded Web UI

The UI is in `code/controller/main/public`:

- `index.html`: single-page shell with `Device`, `System`, and `Settings` tabs.
- `script.js`: API client, state polling, Wiegand/RF registration UI, PIN user
  CRUD, Wi-Fi/server forms, and control bindings.
- `style.css`: responsive Material-inspired styling.
- `favicon.ico`: embedded favicon asset.

The UI intentionally loads keypad users and logs through dedicated endpoints so
large lists do not inflate `/api/state` or fragment ESP32 heap. Wiegand polling
is active while Wiegand registration is active. RF polling uses state refresh
while RF registration is active.

## Test Suite: `code/controller/tests`

The active firmware has a Node.js test suite.

Setup:

```bash
cd code/controller/tests
npm install
npx playwright install chromium
```

Common commands:

```bash
npm test
npm run test:api
npm run test:quick
npm run test:ui
npm run test:stress
npm run test:physical
```

Useful environment variables:

- `DEVICE_URL`: defaults to `http://192.168.4.1`
- `DEVICE_AP_SSID`: optional AP SSID hint
- `HEADLESS=false`: run Playwright visibly

Test areas:

- firmware build artifact checks
- API state and monitoring endpoints
- API configuration toggles and user CRUD
- stress/bulk user and toggle tests
- Playwright UI tests
- interactive physical hardware tests

The generated report is `code/controller/tests/test-report.html`.

## Reverse Tunnel: `code/tunnel`

`code/tunnel` is a Node.js 18+ reverse HTTP tunnel server for ESP32 devices.

Key files:

- `src/server.js`: TCP tunnel server and HTTP proxy.
- `src/protocol.js`: frame parser/writer.
- `src/mock-client.js`: mock ESP32 tunnel client for local tests.
- `tests/rest-smoke.js`: smoke test through the tunnel against a real device.
- `Dockerfile` and `docker-compose.yml`: containerized deployment.
- `systemd/esp32-tunnel.service`: systemd unit.
- `scripts/install-service.sh` and `scripts/uninstall-service.sh`: Ubuntu
  service install/remove scripts.
- `env.example`: deployment configuration template.
- `device-ui/README.md`: explains optional tunnel-hosted static UI assets.

Tunnel protocol:

1. Device opens outbound TCP connection to the gateway, default port `9001`.
2. Server sends an assignment frame with a device ID.
3. Device can identify with a stable ID.
4. External clients request `http://host:9000/device/<deviceId>/<path>`.
5. The server frames the HTTP request and forwards it to the device.
6. The device returns `httpResponse`, `httpResponseStart`/`Chunk`/`End`, or
   `httpError` frames.

Frame format:

```text
[4-byte big-endian JSON header length][JSON header][body bytes]
```

The tunnel server can optionally serve copied static UI assets from
`code/tunnel/device-ui` so large HTML/CSS/JS files do not have to pass through
the ESP32 tunnel during page load.

## Alternate Firmware: `code/controller_mini`

`code/controller_mini` is an alternate ESP32-S3 firmware tree. It appears to be
an experimental refactor rather than the primary controller firmware.

Notable differences:

- `main/main.c` delegates networking, OTA, service startup, and configuration
  to manager modules.
- `services/service_manager.c` starts all peripheral services through named
  wrappers.
- Additional manager/service files exist for `network_manager`,
  `ota_manager`, `config_manager`, `utilities`, and `radar`.
- `radar_main(4, 5, 8, 9)` starts two UART radar channels.
- It includes flash/setup notes in `USAGE.md`,
  `ESP32-S3_FLASH_GUIDE.md`, and `REAL_FLASH_SOLUTION.md`.

This tree shares many service names with `code/controller`, but it has separate
source files and should not be assumed to behave identically.
