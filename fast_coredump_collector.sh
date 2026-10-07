#!/bin/bash
# /usr/local/bin/fast_coredump_collector
# Arguments passed by kernel: %e (executable name), %p (PID), %t (timestamp)
EXE_NAME="${1:-unknown}"
CRASH_PID="${2:-0}"
CRASH_TIME="${3:-$(date +%s)}"

OUTPUT_DIR="/tmp/crash_dumps"
mkdir -p "$OUTPUT_DIR"
chmod 777 "$OUTPUT_DIR"

OUTPUT_FILE="$OUTPUT_DIR/core.${EXE_NAME}.${CRASH_PID}.zst"
LOG_FILE="/tmp/crash_dumps/collector.log"

START_MS=$(date +%s%3N)
echo "[$(date '+%H:%M:%S.%3N')] [COLLECTOR] Invoked for ${EXE_NAME} (PID: ${CRASH_PID})" >> "$LOG_FILE"

# Stream directly from stdin (kernel pipe) through ultra-fast zstd level 1 compression into disk
# --fast=1 achieves 1+ GB/sec throughput directly off memory bus!
/usr/bin/zstd -1 -q -o "$OUTPUT_FILE"

END_MS=$(date +%s%3N)
ELAPSED=$((END_MS - START_MS))
SIZE=$(du -h "$OUTPUT_FILE" 2>/dev/null | awk '{print $1}')

echo "[$(date '+%H:%M:%S.%3N')] [COLLECTOR] Completed in ${ELAPSED} ms! File: ${OUTPUT_FILE} (${SIZE})" >> "$LOG_FILE"
