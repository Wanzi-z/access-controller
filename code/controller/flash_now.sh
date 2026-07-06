#!/bin/bash
# Build and flash the ESP32-S3 with the normal IDF/esptool connection output.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

PORT="${ESP_PORT:-/dev/ttyUSB0}"

# Load IDF
source /home/andy/esp/esp-idf/export.sh >/dev/null 2>&1

echo "========================================="
echo "ESP32-S3 Build + Flash"
echo "========================================="
echo ""
echo "Port: $PORT"
echo ""
echo "This script builds first, then enters download mode with the CP2102N"
echo "DTR/RTS sequence used by this controller board. No PROG/BOOT press"
echo "should be needed. The Connecting dots are real esptool retries."
echo ""

if [ ! -e "$PORT" ]; then
    echo "ERROR: Serial port does not exist: $PORT"
    echo "Set ESP_PORT=/dev/ttyUSBx if the controller is on another port."
    exit 2
fi

if fuser "$PORT" >/dev/null 2>&1; then
    echo "ERROR: Serial port is busy: $PORT"
    fuser -v "$PORT" || true
    exit 3
fi

echo "Building firmware..."
idf.py build

echo ""
echo "Entering ESP32-S3 download mode without button press..."
python - "$PORT" <<'PY'
import serial
import sys
import time

port = sys.argv[1]
ser = serial.Serial(port, 115200, timeout=0.1, dsrdtr=False, rtscts=False)

# This board's CP2102N reset wiring is inverted relative to esptool's
# default sequence. These states were verified on the target hardware:
# DTR=False and RTS=True/False enters download mode on this board.
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

echo "Flashing firmware..."
echo ""
(
    cd build
    python -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
        --before no_reset --after no_reset \
        write_flash "@flash_args"
)

echo ""
echo "Resetting into app mode..."
python - "$PORT" <<'PY'
import serial
import sys
import time

port = sys.argv[1]
ser = serial.Serial(port, 115200, timeout=0.1, dsrdtr=False, rtscts=False)
# App boot pulse verified on the target hardware.
ser.dtr = False
ser.rts = True
time.sleep(0.25)
ser.rts = False
time.sleep(0.2)
ser.close()
PY

echo ""
echo "========================================="
echo "FLASH COMPLETE"
echo "The device should now reboot with new firmware."
echo "========================================="
echo ""
echo "To monitor serial output:"
echo "  cd $SCRIPT_DIR && source ~/esp/esp-idf/export.sh && idf.py -p $PORT monitor"
