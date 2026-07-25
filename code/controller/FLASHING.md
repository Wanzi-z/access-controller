# Flashing & updating the Access Controller

There are three ways to update the firmware. **Day-to-day updates need no buttons and no USB cable — use OTA.**

## 1. OTA over Wi‑Fi — the normal, buttonless path ✅

No PROG button, no REBOOT button, no USB. The device just has to be reachable on the LAN.

```bash
cd code/controller
source ~/esp/esp-idf/export.sh
idf.py build                                   # produces build/controller.bin
python tools/ota_client.py --host 192.168.1.115 --yes
```

`ota_client.py` uploads `build/controller.bin` to `POST /api/ota/upload`, the device writes it to the
inactive OTA slot (app0/app1), verifies it, and reboots into it automatically. Rollback is enabled, so a
bad image reverts on its own. This is the method to use every time.

> If you omit `--host`, it scans the LAN and lists any controllers it finds.

### 1a. OTA when the device is off-LAN — through Device Manager

When the controller is on a NAT'd / remote network (e.g. `ec3409`) and not
LAN-reachable, OTA still works buttonlessly **through the Device Manager tunnel
proxy** — the DM streams the image to the device over the reverse tunnel:

```bash
idf.py build                                    # build/controller.bin
DMID=f7f34b9f-7ee3-5519-a8db-f703581931c0       # the device's Device Manager id
curl -X POST http://localhost:8102/api/devices/$DMID/access-controller/ota \
  -H 'x-firmware-filename: controller.bin' \
  --data-binary @build/controller.bin
```

The device does a "streamed tunnel OTA to partition app0/app1", marks the image
valid after a clean boot, and reboots into it. See `docs/CLOUD_TUNNEL.md` →
*Device control tunnel & Device Manager HTTP proxy* for the full path and ops.

## 2. Buttonless reset to the app (recover a stuck/download-mode chip)

> **Serial-port gotcha:** the controller (ESP32-S3) enumerates as the **CP210x on
> `/dev/ttyUSB0`**. A `/dev/ttyACM0` on the same host is a *different* board (an
> ESP32-C6 dev unit), not the controller — don't flash or reset it by mistake.


The USB‑serial **RTS line is wired to EN (reset)**, so the board can be rebooted into the running app over
USB with no button press:

```bash
code/controller/reset_to_app.sh          # pulses EN via RTS, GPIO0 held high => boots the app
```

Use this if the chip was left sitting in download mode (silent, off the network) after a serial flash.

## 3. Serial flash — recovery only (requires the PROG button)

Only needed when the device is bricked/unreachable and OTA is impossible. **This board's USB‑serial DTR line
is *not* wired to GPIO0**, so esptool cannot pull the chip into download mode on its own — entering download
mode requires the physical **PROG** button. (Reset/EN *is* automated; only download-mode entry needs the button.)

```bash
cd code/controller && source ~/esp/esp-idf/export.sh
# Arm the flasher, then on the board: hold PROG -> tap REBOOT -> release PROG
ESP_PORT=/dev/ttyUSB0 ./flash_now.sh
```

To make serial flashing fully buttonless in a future hardware revision, wire the USB‑serial **DTR → GPIO0**
(with the standard two‑transistor auto‑reset network) alongside the existing **RTS → EN**. Until then, keep
the device reachable so OTA (method 1) is always available and the PROG button is never needed.
