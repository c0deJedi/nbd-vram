#!/bin/sh
# nbd-vram-sleep.sh - tear down VRAM swap before sleep, restore it on resume.
# Driven by vram-swap-nbd-suspend.service: ExecStart calls 'pre', ExecStop 'post'.
#
# We teardown/rebuild instead of pause because suspend powers down the GPU and destroys
# the CUDA context backing the NBD device. Swap I/O in flight against a dead context
# blocks forever and deadlocks resume (issue #19). Stopping the service runs the safe
# swapoff + disconnect + daemon exit (freeing all VRAM); starting it builds a fresh
# CUDA context. We reuse the service's stop/start path so the #14 panic-prevention
# (swapoff-fails-aborts-disconnect, long TimeoutStopSec, no SIGKILL) applies here too.

# Marker so 'post' only restarts swap that 'pre' auto-stopped - never resurrects a
# service the user had manually stopped before sleeping.
STATE_FILE=/run/nbd-vram-sleep-disabled

case "$1" in
    pre)
        if systemctl is-active --quiet vram-swap-nbd.service; then
            echo "nbd-vram-sleep: stopping VRAM swap before sleep" >&2
            touch "$STATE_FILE"
            # Blocks until swapoff completes and VRAM is freed, so the GPU suspends clean.
            systemctl stop vram-swap-nbd.service
        fi
        ;;
    post)
        if [ -f "$STATE_FILE" ]; then
            echo "nbd-vram-sleep: restoring VRAM swap after resume" >&2
            rm -f "$STATE_FILE"
            # Never start a disabled unit - enable/disable is the sole on/off switch.
            systemctl is-enabled --quiet vram-swap-nbd.service || exit 0
            # Fresh daemon -> fresh CUDA context (ExecStartPre wakes the GPU itself).
            systemctl start vram-swap-nbd.service
        fi
        ;;
    *)
        echo "usage: $0 {pre|post}" >&2
        exit 1
        ;;
esac
