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
echo "This script builds first, then gives a countdown before flashing."
echo "Hold PROG/BOOT during the countdown and keep holding until"
echo "'Chip is ESP32-S3' appears. The Connecting dots are real esptool retries."
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
echo "Get ready to hold PROG/BOOT."
for seconds in 5 4 3 2 1; do
    echo "Starting flash in $seconds..."
    sleep 1
done

echo ""
echo "Hold PROG/BOOT now."
echo "Flashing firmware..."
echo ""
idf.py -p "$PORT" flash

echo ""
echo "========================================="
echo "FLASH COMPLETE"
echo "The device should now reboot with new firmware."
echo "========================================="
echo ""
echo "To monitor serial output:"
echo "  cd $SCRIPT_DIR && source ~/esp/esp-idf/export.sh && idf.py -p $PORT monitor"
