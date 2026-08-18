#!/bin/bash
# install.sh - Install nbd-vram VRAM swap (CUDA + NBD, no kernel module needed)
# Run once as root. Survives kernel/driver updates automatically.
#
# Architecture:
#   nbd-vram daemon  - allocates VRAM via CUDA, serves NBD protocol on Unix socket
#   nbd kernel module (built-in) + nbd-client - exposes /dev/nbd0 as a block device
#   systemd service  - manages startup, mkswap, swapon/swapoff lifecycle
#
# Why not the vram_swap kernel module?
#   nvidia_p2p_get_pages_persistent is gated on GeForce/consumer GPUs. The P2P
#   API only works on Quadro/datacenter SKUs. This NBD approach works on any
#   CUDA-capable GPU with no NVIDIA kernel symbols or P2P dependency.

set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

# Remember previously-installed knobs so a reinstall can default to them
PREV_ALLOC=$(grep -oE 'VRAM_SETUP_SIZE_MB=[0-9]+' /etc/systemd/system/vram-swap-nbd.service 2>/dev/null | grep -oE '[0-9]+$' || true)
PREV_COMPRESS=$(sed -n 's/^Environment=VRAM_COMPRESS=\([0-9][0-9]*\).*/\1/p' /etc/systemd/system/vram-swap-nbd.service 2>/dev/null | head -1 || true)
PREV_RATIO=$(sed -n 's/^Environment=VRAM_COMPRESS_RATIO=\([0-9]\+\(\.[0-9]\)\?\).*/\1/p' /etc/systemd/system/vram-swap-nbd.service 2>/dev/null | head -1 || true)

echo "=== nbd-vram installer ==="
echo "Source: $SRC_DIR"

# Detect an existing install so we can restart it at the end (upgrade path).
# Stop it cleanly via systemd first so ExecStop runs the safe swapoff before we
# replace the binary - never pkill a swap-backing daemon out from under active swap.
SERVICE_WAS_ACTIVE=0
if systemctl is-active --quiet vram-swap-nbd.service 2>/dev/null; then
    SERVICE_WAS_ACTIVE=1
    echo "[pre] vram-swap-nbd.service is running - stopping for upgrade..."
    systemctl stop vram-swap-nbd.service || true
    sleep 1
fi

# Mop up any stray (non-systemd) test instance still holding VRAM
if pgrep -x nbd-vram &>/dev/null; then
    echo "[pre] stopping stray nbd-vram instance..."
    bash "$SRC_DIR/nbd-vram-disconnect.sh" 2>/dev/null || true
    pkill -x nbd-vram 2>/dev/null || true
    sleep 1
fi

# Install config if not already present (no-clobber - preserves user edits on reinstall)
if [ ! -f /etc/nbd-vram.conf ]; then
    install -m 644 "$SRC_DIR/nbd-vram.conf" /etc/nbd-vram.conf

    echo ""
    printf "Enable power-aware management? Auto-disable VRAM swap on battery/low power [y/N]: "
    read -r PM_REPLY || PM_REPLY=""
    if [ "$PM_REPLY" = "y" ] || [ "$PM_REPLY" = "Y" ]; then
        sed -i 's/VRAM_POWER_MANAGEMENT=0/VRAM_POWER_MANAGEMENT=1/' /etc/nbd-vram.conf
        printf "Disable when unplugged from AC? [Y/n]: "
        read -r BAT_REPLY || BAT_REPLY=""
        if [ "$BAT_REPLY" = "n" ] || [ "$BAT_REPLY" = "N" ]; then
            sed -i 's/VRAM_DISABLE_ON_BATTERY=1/VRAM_DISABLE_ON_BATTERY=0/' /etc/nbd-vram.conf
            printf "Disable below battery %% (0 = never) [20]: "
            read -r THRESH || THRESH=""
            THRESH=${THRESH:-20}
            sed -i "s/VRAM_BATTERY_THRESHOLD=20/VRAM_BATTERY_THRESHOLD=${THRESH}/" /etc/nbd-vram.conf
        fi
        echo "Power management enabled. Edit /etc/nbd-vram.conf to change settings later."
    else
        echo "Power management left disabled. Edit /etc/nbd-vram.conf to enable later."
    fi
    echo ""
fi

# Ensure nbd-client is installed
echo "[1/4] Checking dependencies..."
if ! command -v nbd-client &>/dev/null; then
    echo "      installing nbd-client..."
    apt-get install -y nbd-client
fi
echo "      OK"

# Build the daemon
echo "[2/4] Building nbd-vram daemon..."
gcc -O2 -Wall -o "$SRC_DIR/nbd-vram" "$SRC_DIR/nbd-vram.c" -ldl -lpthread
echo "      OK"

# Install binary and service
echo "[3/4] Installing binaries and systemd unit..."
install -m 755 "$SRC_DIR/nbd-vram"                          /usr/local/bin/nbd-vram
install -m 755 "$SRC_DIR/nbd-vram-compression-status.sh"    /usr/local/bin/nbd-vram-compression-status.sh
install -m 755 "$SRC_DIR/nbd-vram-connect.sh"               /usr/local/bin/nbd-vram-connect.sh
install -m 755 "$SRC_DIR/nbd-vram-disconnect.sh"            /usr/local/bin/nbd-vram-disconnect.sh
install -m 644 "$SRC_DIR/systemd/vram-swap-nbd.service"          /etc/systemd/system/
install -m 755 "$SRC_DIR/nbd-vram-sleep.sh"                     /usr/local/bin/nbd-vram-sleep.sh
install -m 644 "$SRC_DIR/systemd/vram-swap-nbd-suspend.service" /etc/systemd/system/
install -m 755 "$SRC_DIR/nbd-vram-power-check.sh"               /usr/local/bin/nbd-vram-power-check.sh
install -m 644 "$SRC_DIR/systemd/nbd-vram-power-check.service"  /etc/systemd/system/
install -m 644 "$SRC_DIR/systemd/nbd-vram-battery-watch.service" /etc/systemd/system/
install -m 644 "$SRC_DIR/systemd/nbd-vram-battery-watch.timer"  /etc/systemd/system/
mkdir -p /etc/udev/rules.d
install -m 644 "$SRC_DIR/udev/99-nbd-vram-power.rules"          /etc/udev/rules.d/

# Disable old P2P-based services if present
systemctl disable --now vram-setup.service  2>/dev/null || true
systemctl disable --now vram-swapon.service 2>/dev/null || true
systemctl disable --now vram-swap2.service  2>/dev/null || true
echo "      OK"

# Patch thread/connection count to match available CPUs on this machine
NCPU=$(nproc)
sed -i "s/VRAM_NBD_THREADS=.*/VRAM_NBD_THREADS=${NCPU}/" /etc/systemd/system/vram-swap-nbd.service
sed -i "s/VRAM_NBD_CONNECTIONS=.*/VRAM_NBD_CONNECTIONS=${NCPU}/" /etc/systemd/system/vram-swap-nbd.service
echo "      threads/connections set to ${NCPU} (nproc)"

# Ask how much VRAM to dedicate to swap (interactive only, validated). On a
# non-interactive run (e.g. tooling) the service-file default is left untouched.
if [ -t 0 ]; then
    TOTAL_VRAM=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ')
    # Is this card driving a display? Even one that does NOT drive the display
    # (hybrid laptop, or a workstation with a separate display GPU) is still
    # initialised by the display server for PRIME offload at boot and needs some
    # VRAM - leaving too little crashes Xorg on a cold boot. The display server's
    # need is roughly fixed (a couple of GiB), not proportional to card size, so
    # leave a fixed headroom: ~1 GiB for an offload-only card, ~3 GiB for one that
    # renders the desktop. This doubles as the hard cap the prompt enforces below.
    DISP=$(nvidia-smi --query-gpu=display_active --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ')
    REC=""
    if [ -n "$TOTAL_VRAM" ]; then
        if [ "$DISP" = "Disabled" ]; then
            REC=$(( TOTAL_VRAM - 1024 ))   # offload-only: leave ~1 GiB for the display server's init
        else
            REC=$(( TOTAL_VRAM - 3072 ))   # drives the display: leave ~3 GiB for the desktop
        fi
        # too small to leave safe headroom AND still dedicate the 1024 minimum
        [ "$REC" -lt 1024 ] && { REC=1024; SMALL=1; }
    fi
    CAP="$REC"   # hard cap for the prompt = the safe recommendation for this card
    # default to the previous value only if it is within the cap, else the recommendation
    if [ -n "$PREV_ALLOC" ] && { [ -z "$CAP" ] || [ "$PREV_ALLOC" -le "$CAP" ]; }; then
        DEF="$PREV_ALLOC"
    else
        DEF="${REC:-7168}"
    fi
    echo ""
    if [ -n "$REC" ]; then
        echo "Your GPU reports ${TOTAL_VRAM} MiB of VRAM."
        [ -n "${SMALL:-}" ] && echo "Note: this GPU is small; dedicating VRAM may leave too little for the display."
        echo "Recommended and maximum: ${REC} MiB. Pick less for more headroom. Modify"
        echo "VRAM_SETUP_SIZE_MB in the service file at your own risk to allocate more."
    fi
    while :; do
        printf "VRAM to allocate for swap, in MiB [%s]: " "$DEF"
        read -r ALLOC || ALLOC=""
        ALLOC=${ALLOC:-$DEF}
        case "$ALLOC" in
            ''|*[!0-9]*) echo "  please enter a whole number"; continue ;;
        esac
        if [ "$ALLOC" -lt 1024 ]; then echo "  too small (minimum 1024 MiB)"; continue; fi
        if [ -n "$CAP" ] && [ "$ALLOC" -gt "$CAP" ]; then
            echo "  too high - the safe maximum here is ${CAP} MiB, to leave the display server its VRAM"
            continue
        fi
        break
    done
    sed -i "s/VRAM_SETUP_SIZE_MB=.*/VRAM_SETUP_SIZE_MB=${ALLOC}/" /etc/systemd/system/vram-swap-nbd.service
    echo "      VRAM allocation set to ${ALLOC} MiB"

    echo ""
    echo "lz4 compression packs swap pages in VRAM so the swap device can be larger"
    echo "than the CUDA allocation (default 2.0x). Needs liblz4. Slightly more CPU per fault."
    if [ "${PREV_COMPRESS:-0}" = "1" ]; then
        printf "Enable lz4 compression of VRAM swap? [Y/n]: "
        read -r COMP_REPLY || COMP_REPLY=""
        if [ "$COMP_REPLY" = "n" ] || [ "$COMP_REPLY" = "N" ]; then
            COMPRESS=0
        else
            COMPRESS=1
        fi
    else
        printf "Enable lz4 compression of VRAM swap? [y/N]: "
        read -r COMP_REPLY || COMP_REPLY=""
        if [ "$COMP_REPLY" = "y" ] || [ "$COMP_REPLY" = "Y" ]; then
            COMPRESS=1
        else
            COMPRESS=0
        fi
    fi
    RATIO=${PREV_RATIO:-2.0}
    if [ "$COMPRESS" = "1" ]; then
        while :; do
            printf "Compression ratio (logical size / VRAM, 1.0-8.0, one decimal) [%s]: " "$RATIO"
            read -r RATIO_REPLY || RATIO_REPLY=""
            RATIO_REPLY=${RATIO_REPLY:-$RATIO}
            case "$RATIO_REPLY" in
                [1-7]|[1-7].[0-9]|8|8.0) ;;
                *)
                echo "  please enter X or X.Y from 1.0 to 8.0 (examples: 2, 2.5, 7.9)"
                continue
                ;;
            esac
            if [ "$RATIO_REPLY" = "8" ]; then
                RATIO_REPLY="8.0"
            elif expr "$RATIO_REPLY" : '^[1-7]$' >/dev/null; then
                RATIO_REPLY="${RATIO_REPLY}.0"
            fi
            RATIO=$RATIO_REPLY
            break
        done
        if ! ldconfig -p 2>/dev/null | grep -q 'liblz4.so.1'; then
            echo "      installing liblz4 (needed for VRAM_COMPRESS=1)..."
            apt-get install -y liblz4-1 || {
                echo "      warning: could not install liblz4-1; the service will fail to start until liblz4.so.1 is present" >&2
            }
        fi
    fi
    sed -i "s/^Environment=VRAM_COMPRESS=[0-9][0-9]*/Environment=VRAM_COMPRESS=${COMPRESS}/" /etc/systemd/system/vram-swap-nbd.service
    sed -i "s/^Environment=VRAM_COMPRESS_RATIO=[0-9]\+\(\.[0-9]\)\?/Environment=VRAM_COMPRESS_RATIO=${RATIO}/" /etc/systemd/system/vram-swap-nbd.service
    if [ "$COMPRESS" = "1" ]; then
        EXPORT_MIB=$(awk -v a="$ALLOC" -v r="$RATIO" 'BEGIN { printf "%d", a * r }')
        echo "      lz4 compression on, ratio ${RATIO} (${ALLOC} MiB VRAM -> ${EXPORT_MIB} MiB swap)"
    else
        echo "      lz4 compression off (1:1 VRAM mapping)"
    fi
else
    # Non-interactive reinstall: keep previous compress knobs if present
    if [ -n "$PREV_COMPRESS" ]; then
        sed -i "s/^Environment=VRAM_COMPRESS=[0-9][0-9]*/Environment=VRAM_COMPRESS=${PREV_COMPRESS}/" /etc/systemd/system/vram-swap-nbd.service
    fi
    if [ -n "$PREV_RATIO" ]; then
        sed -i "s/^Environment=VRAM_COMPRESS_RATIO=[0-9]\+\(\.[0-9]\)\?/Environment=VRAM_COMPRESS_RATIO=${PREV_RATIO}/" /etc/systemd/system/vram-swap-nbd.service
    fi
fi

# Enable and (re)start
echo "[4/4] Enabling vram-swap-nbd.service..."
systemctl daemon-reload
systemctl enable vram-swap-nbd.service
# Tear down VRAM swap before the GPU powers off on suspend, restore it on resume.
# Always-on: it's a correctness fix (issue #19), and on a machine that never
# suspends the hook simply never fires.
systemctl enable vram-swap-nbd-suspend.service
systemctl enable --now nbd-vram-battery-watch.timer
udevadm control --reload-rules
echo "      OK"

# Bring the freshly installed binary live. restart() starts a stopped unit too,
# so this covers both fresh installs and upgrades. Non-fatal: power management may
# legitimately keep it stopped (e.g. on battery), and that should not fail install.
echo "[5/5] Starting vram-swap-nbd.service..."
if [ "$SERVICE_WAS_ACTIVE" = "1" ]; then
    echo "      (upgrade - restarting to load the new binary)"
fi
if systemctl restart vram-swap-nbd.service; then
    sleep 2
    if systemctl is-active --quiet vram-swap-nbd.service; then
        echo "      OK - swap active:"
        swapon --show | sed 's/^/      /'
    else
        echo "      service did not stay active - check: journalctl -u vram-swap-nbd -n 20"
    fi
else
    echo "      start deferred (power management may have it disabled on battery)"
    echo "      start manually with: sudo systemctl start vram-swap-nbd.service"
fi

echo ""
echo "=== Installation complete ==="
echo ""
echo "To check status:"
echo "  systemctl status vram-swap-nbd"
echo "  swapon --show"
echo "  nbd-vram-compression-status.sh"
echo "  journalctl -u vram-swap-nbd -n 20"
echo ""
echo "To uninstall:"
echo "  sudo bash uninstall.sh"
