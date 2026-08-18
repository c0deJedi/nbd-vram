#!/bin/bash
# uninstall.sh - Remove nbd-vram VRAM swap and all associated files

set -e

echo "=== nbd-vram uninstaller ==="

echo "[1/4] Stopping and disabling services..."
systemctl disable --now vram-swap-nbd-suspend.service 2>/dev/null || true
systemctl disable --now vram-swap-nbd.service        2>/dev/null || true
systemctl disable --now nbd-vram-battery-watch.timer 2>/dev/null || true
echo "      OK"

echo "[2/4] Removing binaries..."
rm -f /usr/local/bin/nbd-vram
rm -f /usr/local/bin/nbd-vram-compression-status.sh
rm -f /usr/local/bin/nbd-vram-connect.sh
rm -f /usr/local/bin/nbd-vram-disconnect.sh
rm -f /usr/local/bin/nbd-vram-power-check.sh
rm -f /usr/local/bin/nbd-vram-sleep.sh
echo "      OK"

echo "[3/4] Removing systemd units and udev rules..."
rm -f /etc/systemd/system/vram-swap-nbd.service
rm -f /etc/systemd/system/vram-swap-nbd-suspend.service
rm -f /etc/systemd/system/nbd-vram-power-check.service
rm -f /etc/systemd/system/nbd-vram-battery-watch.service
rm -f /etc/systemd/system/nbd-vram-battery-watch.timer
rm -f /etc/udev/rules.d/99-nbd-vram-power.rules
echo "      OK"

echo "[4/4] Reloading systemd and udev..."
systemctl daemon-reload
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
