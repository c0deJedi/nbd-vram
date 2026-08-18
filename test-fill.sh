#!/bin/bash
# test-fill.sh - write the full VRAM partition, verify a sample, prompt to restore swap
# Requires nbd-vram to already be running (run test-nbd.sh first, or use the installed service)
# Run: sudo bash test-fill.sh
set -e

NBD_DEV=$(cat /run/nbd-vram-dev 2>/dev/null || true)
[ -n "$NBD_DEV" ] || { echo "fill-test: /run/nbd-vram-dev not found - is nbd-vram running?" >&2; exit 1; }
[ -b "$NBD_DEV" ] || { echo "fill-test: $NBD_DEV not a block device" >&2; exit 1; }

SIZE=$(blockdev --getsize64 "$NBD_DEV")
SIZE_MIB=$((SIZE / 1024 / 1024))
echo "=== fill test: $NBD_DEV (${SIZE_MIB} MiB) ==="

# Swapoff first - writing over the header while swap is live causes kernel errors
SWAP_WAS_ON=0
if swapon --show --noheadings | awk '{print $1}' | grep -qxF "$NBD_DEV"; then
    echo "suspending swap on $NBD_DEV..."
    swapoff "$NBD_DEV"
    SWAP_WAS_ON=1
fi

# Always re-activate swap on exit so the partition isn't left stranded
restore_swap() {
    if [ "$SWAP_WAS_ON" = "1" ]; then
        echo ""
        echo "restoring swap on $NBD_DEV..."
        mkswap "$NBD_DEV" >/dev/null
        if [ "${VRAM_COMPRESS:-0}" != "0" ]; then
            swapon "$NBD_DEV" -p "${VRAM_SWAP_PRIORITY:-1500}" --discard=pages
        else
            swapon "$NBD_DEV" -p "${VRAM_SWAP_PRIORITY:-1500}"
        fi
        echo "swap restored:"
        swapon --show
    fi
}
trap restore_swap EXIT

echo "writing ${SIZE_MIB} MiB of zeros..."
# dd exits non-zero when it hits end-of-device - that's expected and means success
time dd if=/dev/zero of="$NBD_DEV" bs=4M conv=fsync status=progress 2>&1 || true
echo ""

echo "verifying 4 MiB sample at offset 0..."
dd if="$NBD_DEV" bs=4M count=1 2>/dev/null > /tmp/fill-read
dd if=/dev/zero  bs=4M count=1 2>/dev/null > /tmp/fill-zero
if cmp -s /tmp/fill-read /tmp/fill-zero; then
    echo "readback OK (all zeros as expected)"
else
    echo "READBACK MISMATCH" >&2
fi
rm -f /tmp/fill-read /tmp/fill-zero

echo ""
printf "Keep swap active after restore? [Y/n] "
read -r ans
if [ "$ans" = "n" ] || [ "$ans" = "N" ]; then
    SWAP_WAS_ON=0
    echo "leaving as raw block device"
fi
# trap fires here and restores swap if SWAP_WAS_ON=1
