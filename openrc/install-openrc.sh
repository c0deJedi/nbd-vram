#!/bin/bash
# install-openrc.sh - Install nbd-vram VRAM swap on OpenRC systems
# Run once as root from the repo root directory.
#
# Requires: openrc, nbd-client, a cron daemon (cronie/vixie-cron/etc)
# Optional: elogind (for suspend/resume support)

set -e
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OPENRC_DIR="$SRC_DIR/openrc"

echo "=== nbd-vram installer (OpenRC) ==="
echo "Source: $SRC_DIR"

if [ "$(id -u)" != "0" ]; then
    echo "Error: this script must be run as root" >&2
    exit 1
fi

# Remember a previously-installed VRAM allocation so a reinstall can default to it
PREV_ALLOC=$(grep -oE 'VRAM_SETUP_SIZE_MB=[0-9]+' /etc/conf.d/nbd-vram-swap 2>/dev/null | grep -oE '[0-9]+$' || true)

# Detect an existing install and stop it cleanly so ExecStop runs swapoff
# before we replace the binary - never kill a swap-backing daemon out from under swap.
SERVICE_WAS_ACTIVE=0
if rc-service nbd-vram-swap status >/dev/null 2>&1; then
    SERVICE_WAS_ACTIVE=1
    echo "[pre] nbd-vram is running - stopping for upgrade..."
    rc-service nbd-vram-swap stop || true
    sleep 1
fi

# Mop up any stray (non-service) test instance still holding VRAM
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
    echo "      nbd-client not found. Install it with your package manager:" >&2
    echo "      Gentoo: emerge sys-block/nbd" >&2
    echo "      Alpine: apk add nbd" >&2
    echo "      Void:   xbps-install nbd" >&2
    exit 1
fi
if ! command -v rc-service &>/dev/null; then
    echo "      rc-service not found - is OpenRC installed?" >&2
    exit 1
fi
echo "      OK"

# Build the daemon
echo "[2/4] Building nbd-vram daemon..."
make -C "$SRC_DIR" nbd-vram
echo "      OK"

# Install binary and OpenRC init files
echo "[3/4] Installing binaries and OpenRC init files..."
install -m 755 "$SRC_DIR/nbd-vram"                          /usr/local/bin/nbd-vram
install -m 755 "$SRC_DIR/nbd-vram-connect.sh"               /usr/local/bin/nbd-vram-connect.sh
install -m 755 "$SRC_DIR/nbd-vram-disconnect.sh"            /usr/local/bin/nbd-vram-disconnect.sh
install -m 755 "$OPENRC_DIR/nbd-vram-power-check.sh"        /usr/local/bin/nbd-vram-power-check.sh
install -m 755 "$OPENRC_DIR/nbd-vram-swap.initd"            /etc/init.d/nbd-vram-swap
# Install conf.d only if not already present (preserves user edits on reinstall)
if [ ! -f /etc/conf.d/nbd-vram-swap ]; then
    install -m 644 "$OPENRC_DIR/nbd-vram-swap.confd"        /etc/conf.d/nbd-vram-swap
fi

# Udev rule: calls nbd-vram-power-check.sh directly (no systemctl)
mkdir -p /etc/udev/rules.d
install -m 644 "$OPENRC_DIR/99-nbd-vram-power.rules"        /etc/udev/rules.d/

# Cron job for battery watch (replaces nbd-vram-battery-watch.timer)
if [ -d /etc/cron.d ]; then
    install -m 644 "$OPENRC_DIR/nbd-vram.cron"              /etc/cron.d/nbd-vram
    echo "      cron job installed to /etc/cron.d/nbd-vram"
else
    echo "      Note: /etc/cron.d not found - install a cron daemon for battery polling."
fi

# Suspend/resume hook via elogind
if [ -d /lib/elogind/system-sleep ]; then
    install -m 755 "$OPENRC_DIR/nbd-vram-sleep.sh"          /lib/elogind/system-sleep/nbd-vram
    echo "      elogind sleep hook installed"
else
    echo "      Note: elogind not found at /lib/elogind - suspend/resume teardown won't run."
    echo "      Install elogind or wire up $OPENRC_DIR/nbd-vram-sleep.sh manually."
fi

echo "      OK"

# Patch thread/connection count to match available CPUs
NCPU=$(nproc)
sed -i "s/VRAM_NBD_THREADS=.*/VRAM_NBD_THREADS=${NCPU}/"       /etc/conf.d/nbd-vram-swap
sed -i "s/VRAM_NBD_CONNECTIONS=.*/VRAM_NBD_CONNECTIONS=${NCPU}/" /etc/conf.d/nbd-vram-swap
echo "      threads/connections set to ${NCPU} (nproc)"

# Ask how much VRAM to dedicate to swap
if [ -t 0 ]; then
    TOTAL_VRAM=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ')
    DISP=$(nvidia-smi --query-gpu=display_active --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ')
    REC=""
    if [ -n "$TOTAL_VRAM" ]; then
        if [ "$DISP" = "Disabled" ]; then
            REC=$(( TOTAL_VRAM - 1024 ))
        else
            REC=$(( TOTAL_VRAM - 3072 ))
        fi
        [ "$REC" -lt 1024 ] && { REC=1024; SMALL=1; }
    fi
    CAP="$REC"
    if [ -n "$PREV_ALLOC" ] && { [ -z "$CAP" ] || [ "$PREV_ALLOC" -le "$CAP" ]; }; then
        DEF="$PREV_ALLOC"
    else
        DEF="${REC:-7168}"
    fi
    echo ""
    if [ -n "$REC" ]; then
        echo "Your GPU reports ${TOTAL_VRAM} MiB of VRAM."
        [ -n "${SMALL:-}" ] && echo "Note: this GPU is small; dedicating VRAM may leave too little for the display."
        echo "Recommended and maximum: ${REC} MiB."
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
            echo "  too high - safe maximum here is ${CAP} MiB"
            continue
        fi
        break
    done
    sed -i "s/VRAM_SETUP_SIZE_MB=.*/VRAM_SETUP_SIZE_MB=${ALLOC}/" /etc/conf.d/nbd-vram-swap
    echo "      VRAM allocation set to ${ALLOC} MiB"
fi

# Enable and (re)start
echo "[4/4] Enabling nbd-vram service..."
rc-update add nbd-vram-swap default
udevadm control --reload-rules
echo "      OK"

echo ""
echo "Starting nbd-vram..."
if [ "$SERVICE_WAS_ACTIVE" = "1" ]; then
    echo "(upgrade - restarting to load the new binary)"
fi
if rc-service nbd-vram-swap start; then
    if rc-service nbd-vram-swap status >/dev/null 2>&1; then
        echo "OK - swap active:"
        swapon --show | sed 's/^/  /'
    else
        echo "service did not stay active - check: rc-service nbd-vram-swap status"
    fi
else
    echo "start deferred (power management may have it disabled on battery)"
    echo "start manually with: rc-service nbd-vram-swap start"
fi

echo ""
echo "=== Installation complete ==="
echo ""
echo "To check status:"
echo "  rc-service nbd-vram-swap status"
echo "  swapon --show"
echo ""
echo "To uninstall:"
echo "  sudo bash openrc/uninstall-openrc.sh"
