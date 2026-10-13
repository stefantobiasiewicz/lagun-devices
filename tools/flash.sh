#!/usr/bin/env bash
# Flashing an image from macOS onto the board.
#
# Building happens in the container, flashing here — because Docker on macOS runs in a
# virtual machine that USB does not reach. The container leaves finished files in
# apps/<app>/build/, and from here nothing but esptool is needed.
#
#   brew install esptool      (or: pip install esptool)
#
set -euo pipefail

APP="${1:-}"
PORT="${2:-}"

if [ -z "$APP" ]; then
    echo "usage: $0 <key|key3|output|out8|env|epd> [port]" >&2
    echo >&2
    echo "Ports visible right now:" >&2
    ls /dev/cu.usbmodem* /dev/cu.wchusbserial* 2>/dev/null | sed 's/^/  /' >&2 \
        || echo "  none — is the board plugged in?" >&2
    exit 2
fi

DIR="$(cd "$(dirname "$0")/.." && pwd)/apps/$APP"
BUILD="$DIR/build"

if [ ! -d "$BUILD" ]; then
    echo "no $BUILD — build it in the container first:" >&2
    echo "  cd apps/$APP && idf.py set-target esp32c3 && idf.py build" >&2
    exit 1
fi

if [ -z "$PORT" ]; then
    PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
    if [ -z "$PORT" ]; then
        echo "no port found — pass it as the second argument" >&2
        exit 1
    fi
    echo "port: $PORT"
fi

# The addresses come from the default ESP32-C3 partition layout. Should the project
# ever get a partitions.csv of its own, this place has to be corrected — flash_args in
# build/ tells the truth.
esptool.py --chip esp32c3 --port "$PORT" --baud 921600 \
    --before default_reset --after hard_reset write_flash \
    -z --flash_mode dio --flash_freq 80m --flash_size detect \
    0x0     "$BUILD/bootloader/bootloader.bin" \
    0x8000  "$BUILD/partition_table/partition-table.bin" \
    0x10000 "$BUILD/lagun-$APP.bin"

echo
echo "flashed. Log:  tools/monitor.sh $APP      (quit: Ctrl-])"
