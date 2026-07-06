#!/bin/bash
# Smart flash script - waits for ESP32-S3 manual download mode entry
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Load IDF
source /home/andy/esp/esp-idf/export.sh >/dev/null 2>&1

echo "========================================="
echo "ESP32-S3 Smart Flasher"
echo "========================================="
echo ""
echo "This board requires MANUAL download mode entry:"
echo ""
echo "   👉 1. HOLD DOWN the BOOT button"
echo "   👉 2. While holding BOOT, press & release RESET"
echo "   👉 3. RELEASE the BOOT button"
echo ""
echo "Waiting for download mode..."

DETECTED=0
for i in $(seq 1 120); do
    # Try esptool chip_id detection which works even in download mode
    if python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 115200 \
        --before no_reset_no_sync --after no_reset chip_id >/dev/null 2>&1; then
        DETECTED=1
        break
    fi
    echo -n "."
    sleep 0.5
done

echo ""

if [ "$DETECTED" -eq 0 ]; then
    echo "❌ Timeout waiting for download mode."
    echo "   Please try again and make sure to:"
    echo "   1. HOLD BOOT button"
    echo "   2. Press and release RESET while holding BOOT"
    echo "   3. Release BOOT button"
    exit 1
fi

echo "✅ ESP32-S3 detected in download mode!"
echo "🔥 FLASHING NOW..."
echo ""

python -m esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 \
    --before no_reset_no_sync --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 build/bootloader/bootloader.bin \
    0x10000 build/controller.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xe000 build/ota_data_initial.bin

echo ""
echo "========================================="
echo "✅ FLASH COMPLETE!"
echo "The device should now reboot with new firmware."
echo "========================================="
echo ""
echo "To monitor serial output:"
echo "  cd $SCRIPT_DIR && source ~/esp/esp-idf/export.sh && idf.py -p /dev/ttyUSB0 monitor"
