#!/bin/bash
# uninstall-openrc.sh - Remove nbd-vram VRAM swap (OpenRC install)

set -e

echo "=== nbd-vram uninstaller (OpenRC) ==="

if [ "$(id -u)" != "0" ]; then
    echo "Error: this script must be run as root" >&2
    exit 1
fi

echo "[1/4] Stopping and disabling service..."
rc-service nbd-vram-swap stop 2>/dev/null || true
rc-update del nbd-vram-swap   2>/dev/null || true
echo "      OK"

echo "[2/4] Removing binaries..."
rm -f /usr/local/bin/nbd-vram
rm -f /usr/local/bin/nbd-vram-connect.sh
rm -f /usr/local/bin/nbd-vram-disconnect.sh
rm -f /usr/local/bin/nbd-vram-power-check.sh
echo "      OK"

echo "[3/4] Removing init files, cron, sleep hook, and udev rules..."
rm -f /etc/init.d/nbd-vram-swap
rm -f /etc/conf.d/nbd-vram-swap
rm -f /etc/cron.d/nbd-vram
rm -f /lib/elogind/system-sleep/nbd-vram
rm -f /etc/udev/rules.d/99-nbd-vram-power.rules
echo "      OK"

echo "[4/4] Reloading udev..."
udevadm control --reload-rules
echo "      OK"

echo ""
printf "Remove /etc/nbd-vram.conf (your power management settings)? [y/N]: "
read -r CONF_REPLY || CONF_REPLY=""
if [ "$CONF_REPLY" = "y" ] || [ "$CONF_REPLY" = "Y" ]; then
    rm -f /etc/nbd-vram.conf
    echo "Config removed."
else
    echo "Config kept at /etc/nbd-vram.conf."
fi

echo ""
echo "=== Uninstall complete ==="
