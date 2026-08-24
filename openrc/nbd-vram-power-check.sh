#!/bin/sh
# nbd-vram-power-check.sh - stop/start VRAM swap based on power state
# OpenRC version: uses rc-service instead of systemctl.
# Triggered by udev on AC plug/unplug and by /etc/cron.d/nbd-vram every 60s.
# No-op when VRAM_POWER_MANAGEMENT=0 (default).

CONF=/etc/nbd-vram.conf
STATE_FILE=/run/nbd-vram-power-disabled

# Defaults
VRAM_POWER_MANAGEMENT=0
VRAM_DISABLE_ON_BATTERY=1
VRAM_BATTERY_THRESHOLD=20

# shellcheck source=/dev/null
[ -f "$CONF" ] && . "$CONF"

[ "$VRAM_POWER_MANAGEMENT" = "1" ] || exit 0

# Read AC state; default to online (fail safe - never auto-disable if sysfs unreadable)
AC_ONLINE=1
for f in /sys/class/power_supply/*/online; do
	[ -f "$f" ] || continue
	type_file="${f%online}type"
	[ -f "$type_file" ] || continue
	read -r psu_type < "$type_file"
	[ "$psu_type" = "Mains" ] || continue
	read -r AC_ONLINE < "$f"
	break
done

# Read battery capacity; default to 100 (fail safe - never threshold-disable if no battery)
BATTERY_CAP=100
for f in /sys/class/power_supply/BAT*/capacity; do
	[ -f "$f" ] && read -r BATTERY_CAP < "$f" && break
done

# Determine if VRAM swap should be disabled
SHOULD_DISABLE=0
if [ "$AC_ONLINE" = "0" ]; then
	if [ "$VRAM_DISABLE_ON_BATTERY" = "1" ]; then
		SHOULD_DISABLE=1
	elif [ "$VRAM_BATTERY_THRESHOLD" -gt 0 ] 2>/dev/null && \
	     [ "$BATTERY_CAP" -lt "$VRAM_BATTERY_THRESHOLD" ] 2>/dev/null; then
		SHOULD_DISABLE=1
	fi
fi

if [ "$SHOULD_DISABLE" = "1" ]; then
	if rc-service nbd-vram-swap status >/dev/null 2>&1; then
		echo "nbd-vram-power-check: disabling VRAM swap (AC_ONLINE=$AC_ONLINE battery=${BATTERY_CAP}%)" >&2
		touch "$STATE_FILE"
		rc-service nbd-vram-swap stop
	fi
else
	# Only auto-start if this script previously auto-stopped it.
	# Never auto-start after a manual 'rc-service nbd-vram-swap stop'.
	if [ -f "$STATE_FILE" ] && ! rc-service nbd-vram-swap status >/dev/null 2>&1; then
		echo "nbd-vram-power-check: re-enabling VRAM swap (AC_ONLINE=$AC_ONLINE battery=${BATTERY_CAP}%)" >&2
		rm -f "$STATE_FILE"
		rc-service nbd-vram-swap start
	fi
fi
