#!/bin/bash
# test-nbd.sh - smoke test nbd-vram without installing
# Run: sudo bash test-nbd.sh
set -e

cd "$(dirname "$0")"

echo "=== installing nbd-client if missing ==="
if ! command -v nbd-client &>/dev/null; then
    apt-get install -y nbd-client
fi

echo "=== stopping any previous run ==="
pkill -x nbd-vram 2>/dev/null || true
sleep 0.5
bash nbd-vram-disconnect.sh 2>/dev/null || true
rm -f /run/nbd-vram.sock /run/nbd-vram-dev

echo "=== building ==="
make

echo "=== starting nbd-vram daemon ==="
if [ "${VRAM_COMPRESS:-0}" != "0" ]; then
    echo "      VRAM_COMPRESS=${VRAM_COMPRESS} ratio=${VRAM_COMPRESS_RATIO:-2.0}"
fi
./nbd-vram &
NBD_PID=$!
echo "PID: $NBD_PID"

echo "=== waiting for socket ==="
for i in $(seq 1 60); do
    [ -S /run/nbd-vram.sock ] && break
    sleep 0.5
done
[ -S /run/nbd-vram.sock ] || { echo "TIMEOUT"; kill $NBD_PID; exit 1; }

echo "=== connecting NBD device (no swap yet) ==="
modprobe nbd max_part=0 2>/dev/null || true
NBD_DEV=""
for dev in /dev/nbd{0..15}; do
    [ -b "$dev" ] || continue
    pid=$(cat "/sys/block/$(basename "$dev")/pid" 2>/dev/null || true)
    if [ -z "$pid" ]; then NBD_DEV="$dev"; break; fi
done
[ -n "$NBD_DEV" ] || { echo "no free nbd device" >&2; kill $NBD_PID; exit 1; }
echo "using $NBD_DEV"
echo "$NBD_DEV" > /run/nbd-vram-dev
nbd-client -unix /run/nbd-vram.sock "$NBD_DEV"

echo ""
echo "=== write/read test (1 MiB at offset 0) ==="
dd if=/dev/urandom bs=1M count=1 2>/dev/null > /tmp/vram-test-in
dd if=/tmp/vram-test-in of="$NBD_DEV" bs=1M count=1 conv=fsync 2>/dev/null
dd if="$NBD_DEV" bs=1M count=1 2>/dev/null > /tmp/vram-test-out
if cmp -s /tmp/vram-test-in /tmp/vram-test-out; then
    echo "readback OK"
else
    echo "READBACK MISMATCH" >&2
    rm -f /tmp/vram-test-in /tmp/vram-test-out
    kill $NBD_PID; exit 1
fi
rm -f /tmp/vram-test-in /tmp/vram-test-out

echo ""
echo "=== activating swap ==="
mkswap "$NBD_DEV"
if [ "${VRAM_COMPRESS:-0}" != "0" ]; then
    swapon "$NBD_DEV" -p "${VRAM_SWAP_PRIORITY:-1500}" --discard=pages
else
    swapon "$NBD_DEV" -p "${VRAM_SWAP_PRIORITY:-1500}"
fi

echo ""
swapon --show

echo ""
echo "SUCCESS."
echo "Tear down: sudo bash nbd-vram-disconnect.sh && sudo kill $NBD_PID"
echo "Install:   sudo ./install.sh && sudo systemctl start vram-swap-nbd"
echo "(run tear down before install or the daemon will hold all VRAM)"
