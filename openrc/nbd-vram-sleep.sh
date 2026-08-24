#!/bin/sh
# nbd-vram-sleep.sh - tear down VRAM swap before sleep, restore it on resume.
# OpenRC / elogind version: install to /lib/elogind/system-sleep/nbd-vram (chmod +x).
#
# We teardown/rebuild instead of pause because suspend powers down the GPU and destroys
# the CUDA context backing the NBD device. Swap I/O in flight against a dead context
# blocks forever and deadlocks resume (issue #19). Stopping the service runs the safe
# swapoff + disconnect + daemon exit (freeing all VRAM); starting it builds a fresh
# CUDA context. We reuse the service's stop/start path so the #14 panic-prevention
# (swapoff-fails-aborts-disconnect, long timeout, no SIGKILL) applies here too.

# Marker so 'post' only restarts swap that 'pre' auto-stopped - never resurrects a
# service the user had manually stopped before sleeping.
STATE_FILE=/run/nbd-vram-sleep-disabled

case "$1" in
	pre)
		if rc-service nbd-vram-swap status >/dev/null 2>&1; then
			echo "nbd-vram-sleep: stopping VRAM swap before sleep" >&2
			touch "$STATE_FILE"
			# Blocks until swapoff completes and VRAM is freed, so the GPU suspends clean.
			rc-service nbd-vram-swap stop
		fi
		;;
	post)
		if [ -f "$STATE_FILE" ]; then
			echo "nbd-vram-sleep: restoring VRAM swap after resume" >&2
			rm -f "$STATE_FILE"
			# Fresh daemon -> fresh CUDA context.
			rc-service nbd-vram-swap start
		fi
		;;
	*)
		echo "usage: $0 {pre|post}" >&2
		exit 1
		;;
esac
