#!/bin/bash
TARGET="${1:-disk}"
MEM="${2:-0.2}"

if [ "$TARGET" = "ram" ]; then
    echo "[DEMO] Target: RAM (/dev/shm) | Allocated RAM: ${MEM} GB"
    sudo sysctl -w kernel.core_pattern="/dev/shm/core.%e.%p"
    rm -f /dev/shm/core.*
    ulimit -c unlimited
    ./watcher ram "$MEM"
else
    echo "[DEMO] Target: DISK (/tmp) | Allocated RAM: ${MEM} GB"
    sudo sysctl -w kernel.core_pattern="/tmp/core.%e.%p"
    rm -f /tmp/core.*
    ulimit -c unlimited
    ./watcher disk "$MEM"
fi
