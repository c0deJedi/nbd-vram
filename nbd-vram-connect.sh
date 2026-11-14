#!/bin/bash
# nbd-vram-connect.sh - find a free nbd device, connect, activate swap
# Called by vram-swap-nbd.service ExecStartPost after nbd-vram signals ready.
set -e

modprobe nbd max_part=0 2>/dev/null || true

# Disconnect can return while a block-device opener still holds the old kernel
# configuration. Wait for its release before reconnecting, not just a missing pid.
disconnect_nbd() {
    local dev=$1 status attempt
    nbd-client -d "$dev" 2>/dev/null || true
    for attempt in {1..200}; do
        if nbd-client -c "$dev" >/dev/null 2>&1; then
            sleep 0.05
        else
            status=$?
            if [ "$status" -eq 1 ]; then
                return 0
            fi
            echo "nbd-vram-connect: cannot check disconnect status for $dev" >&2
            return 1
        fi
    done
    echo "nbd-vram-connect: timed out waiting for $dev to disconnect" >&2
    return 1
}

# Clean up any device left over from a previous unclean shutdown
if [ -f /run/nbd-vram-dev ]; then
    PREV=$(cat /run/nbd-vram-dev)
    ACTIVE_SWAPS=$(swapon --show=NAME --noheadings --raw)
    if grep -Fxq -- "$PREV" <<< "$ACTIVE_SWAPS" && ! swapoff "$PREV"; then
        echo "nbd-vram-connect: swapoff $PREV FAILED - preserving device and state" >&2
        exit 1
    fi
    disconnect_nbd "$PREV"
    rm -f /run/nbd-vram-dev
fi

# Find first free /dev/nbdN by checking kernel's pid file for each device
NBD_DEV=""
for dev in /dev/nbd{0..15}; do
    [ -b "$dev" ] || continue
    pid=$(cat "/sys/block/$(basename "$dev")/pid" 2>/dev/null || true)
    if [ -z "$pid" ]; then
        NBD_DEV="$dev"
        break
    fi
done

if [ -z "$NBD_DEV" ]; then
    echo "nbd-vram-connect: no free nbd device found (nbd0-nbd15 all in use)" >&2
    exit 1
fi

echo "nbd-vram-connect: using $NBD_DEV"
# Force-disconnect to clear any stale kernel state from an unclean previous shutdown
disconnect_nbd "$NBD_DEV"
nbd-client -unix /run/nbd-vram.sock "$NBD_DEV" -connections ${VRAM_NBD_CONNECTIONS:-4}
mkswap "$NBD_DEV"
swapon "$NBD_DEV" -p "${VRAM_SWAP_PRIORITY:-1500}"

# Save device name so disconnect script knows what to clean up
echo "$NBD_DEV" > /run/nbd-vram-dev
echo "nbd-vram-connect: swap active on $NBD_DEV"
