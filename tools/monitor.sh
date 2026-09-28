#!/usr/bin/env bash
# Serial log from the board — on macOS, because USB does not reach into the container.
#
# WHY NOT `screen`. On an ESP32-C3 the serial port is provided by the processor itself
# (USB Serial/JTAG), not by a separate chip on the board. On every reset the USB device
# disappears from the system and comes back anew — and `/dev/cu.*`, which `screen` holds
# open, disappears with it. That is why `screen` dies when RST is pressed, and why the
# boot log is never visible: it goes out exactly while the port is gone. This is not a
# fault of the board.
#
# `idf-monitor` survives the port disappearing, waits, and reconnects. It also decodes
# addresses from the panic handler into function names.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-output}"
PORT="${2:-}"

if [ ! -x "$ROOT/.venv/bin/idf-monitor" ]; then
    echo "tools missing — set them up once:" >&2
    echo "  python3 -m venv $ROOT/.venv" >&2
    echo "  $ROOT/.venv/bin/pip install esptool esp-idf-monitor" >&2
    exit 1
fi

ELF="$ROOT/apps/$APP/build/lagun-$APP.elf"
if [ ! -f "$ELF" ]; then
    echo "no $ELF — build it in the container first" >&2
    exit 1
fi

if [ -z "$PORT" ]; then
    PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
    [ -z "$PORT" ] && { echo "no port in sight — is the board plugged in?" >&2; exit 1; }
fi

echo "$PORT — $APP;  quit: Ctrl-]"
# --target is needed because without it the monitor reaches for the Xtensa tools and,
# at the first address it has to decode, says "xtensa-esp32-elf-addr2line: No such
# file". An ESP32-C3 is RISC-V.
exec "$ROOT/.venv/bin/idf-monitor" --port "$PORT" --baud 115200 \
     --target esp32c3 "$ELF"
