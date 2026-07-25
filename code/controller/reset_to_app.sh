#!/bin/bash
# Reboot the ESP32-S3 into its running app over USB serial WITHOUT any button.
# The board's USB-serial RTS line drives EN (reset); GPIO0 is left high so the
# chip boots the app instead of the ROM download mode.
set -euo pipefail
PORT="${ESP_PORT:-/dev/ttyUSB0}"

if [ ! -e "$PORT" ]; then
    echo "ERROR: serial port not found: $PORT (set ESP_PORT=/dev/ttyUSBx)" >&2
    exit 2
fi

python3 - "$PORT" <<'PY'
import serial, sys, time
port = sys.argv[1]
ser = serial.Serial(port, 115200, timeout=0.1, dsrdtr=False, rtscts=False)
ser.dtr = False   # GPIO0 high -> run/app mode (not download)
ser.rts = True    # assert EN (reset)
time.sleep(0.3)
ser.rts = False   # release -> boots the app
time.sleep(0.2)
ser.close()
print("Reset pulse sent on %s; device should boot the app." % port)
PY
