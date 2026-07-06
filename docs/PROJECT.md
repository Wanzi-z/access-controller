# Project Overview

## Device Idea

The access controller is a two-channel door-control system intended for electric
strikes, magnetic locks, and related access-control hardware. It combines:

- lock output control for two channels
- door/contact and lock-sense monitoring
- Wiegand RFID/card/keypad reader input
- PIN users for keypad access
- RF remote/fob learning through a 433 MHz receiver
- exit button, keypad button, fob button, and motion input services
- audible feedback through keypad push/buzzer outputs
- Wi-Fi station mode with AP fallback
- local HTTP API and embedded browser UI
- persistent storage through NVS and SPIFFS
- OTA update check against a configured server
- optional outbound reverse HTTP tunnel

The current active firmware target is the ESP32-S3 controller in
`code/controller`. The older top-level `README.md` still describes an early
ESP32/lws-factory flow, while the current source has moved to an ESP-IDF CMake
project with an embedded web UI and a larger API surface.

## Physical Architecture

The main controller board in `circuits/controller` is organized around:

- `ESP32-S3-WROOM-1` microcontroller module
- `MCP23017` I/O expander on I2C for lock/contact/button inputs and outputs
- Wiegand reader connectors and optocoupled/conditioned inputs
- power input/regulation and PoE-related design material
- W5500 Ethernet schematic sheet, even though the active firmware currently
  focuses on Wi-Fi and tunnel connectivity
- USB programming circuitry and boot/program/factory buttons
- terminal blocks for locks, Wiegand devices, power, and auxiliary inputs
- RXB6/RF module footprint for 433 MHz remotes

The active firmware maps many door-control I/O signals through the MCP23017
instead of direct ESP32 pins. Direct ESP32 pins are still defined for
compatibility in `code/controller/main/services/gpio.c`.

## Runtime Architecture

On boot, `code/controller/main/main.c` performs the main startup sequence:

1. Initialize networking/event loop and NVS.
2. Initialize automation queues and persistent log storage.
3. Ensure a device UUID and token exist in NVS.
4. Load Wi-Fi credentials.
5. Try station mode.
6. If station mode succeeds, load server settings, compare firmware MD5 from the
   server, optionally run OTA, and start the tunnel client.
7. If station mode fails, start Wi-Fi AP mode with an `ac_xxxx` SSID.
8. Initialize GPIO, I2C, MCP23017, authorization, buzzer, Wiegand registry,
   Wiegand reader service, exit, motion, keypad, fob, RF registry, RF receiver,
   lock, and HTTP server services.
9. Initialize SPIFFS and enter a periodic system-status log loop.

The firmware uses an older single-compilation-unit style where `main.c` includes
many service `.c` files directly. That makes the source tree look unusual, but
the service files are still the practical module boundaries.

## Access-Control Behavior

Each lock channel has a control output, contact input, sense input, alert flag,
polarity setting, and persisted enable/arm settings. Services can disarm a lock
temporarily and then re-arm it after their configured delay.

Wiegand long frames, currently `>= 24` bits, are treated as card/RFID codes.
Short frames, currently 4 to 8 bits, are treated as keypad keypresses. PINs are
submitted with `#`; accepted PINs and active Wiegand users disarm the matching
channel. Unauthorized Wiegand codes and invalid PINs trigger keypad beeps.

The Wiegand registry stores users in NVS under the `wiegand_users` key as JSON.
Users have IDs, bit-string codes, names, channel numbers, status, sequence, and
timestamps. Newly captured cards can be pending during registration and promoted
to active when registration stops or a name is saved.

PIN/keypad users are stored as per-user JSON files in SPIFFS using paths like
`/spiffs/user_00001.json`. The firmware keeps an `auth_user_count` index in NVS.

RF remote/fob codes are stored in NVS under `rf_fobs`. The RF registry supports
learned codes, names, mode configuration, channel masks, exit seconds, alert
settings, and actions such as toggle, momentary/exit pulse, arm, and disarm.

## Network and UI Model

The device hosts a local web UI from `code/controller/main/public` and exposes
JSON endpoints under `/api/*`. The UI has three main tabs:

- `Device`: channel controls, RFID/Wiegand users, PIN users, and RF remotes.
- `System`: UUID/status/log display.
- `Settings`: Wi-Fi and server configuration.

When connected to Wi-Fi, the firmware can open an outbound tunnel connection to
the Node.js gateway in `code/tunnel`. External HTTP requests to the tunnel server
are framed, forwarded to the ESP32 over the outbound socket, proxied to the
controller's local HTTP server, then returned through the tunnel.
