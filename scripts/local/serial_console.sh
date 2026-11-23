#!/usr/bin/env bash
# Capture the board's UART console. /dev/ttyUSB0 is a single-reader device, so
# any stale reader is stopped first.
#
# Usage: serial_console.sh [output.log] [seconds]
set -euo pipefail

OUT="${1:-/tmp/charlotte-serial.log}"
SECONDS_TO_RUN="${2:-900}"

pkill -f 'cat /dev/ttyUSB0' 2>/dev/null || true
sleep 0.2

echo "logging to $OUT (${SECONDS_TO_RUN}s, Ctrl-C to stop)"
sg dialout -c "stty -F /dev/ttyUSB0 115200 raw -echo -crtscts -ixon -ixoff; \
               timeout ${SECONDS_TO_RUN} cat /dev/ttyUSB0" | tee "$OUT"
