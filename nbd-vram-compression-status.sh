#!/bin/sh
# nbd-vram-compression-status.sh - show live lz4 compression ratio for VRAM swap
# Reads /run/nbd-vram.status (updated ~1s by the daemon when VRAM_COMPRESS=1).

STATUS=/run/nbd-vram.status

kv() { sed -n "s/^$1=//p" "$STATUS" 2>/dev/null | head -1; }

mib() {
    # bytes -> MiB, integer
    echo $(( $1 / 1024 / 1024 ))
}

if [ ! -f "$STATUS" ]; then
    if [ -S /run/nbd-vram.sock ] || pgrep -x nbd-vram >/dev/null 2>&1; then
        echo "nbd-vram is running without compression (VRAM_COMPRESS=0)."
        echo "Set VRAM_COMPRESS=1 to pack pages in VRAM and see a live ratio here."
        echo ""
        swapon --show 2>/dev/null || true
        exit 0
    fi
    echo "nbd-vram is not running." >&2
    exit 1
fi

compress=$(kv compress)
if [ "$compress" != "1" ]; then
    echo "nbd-vram compression is off."
    exit 0
fi

cfg=$(kv configured_ratio)
cfg10=$(kv configured_ratio_tenths)
vram_bytes=$(kv vram_bytes)
export_bytes=$(kv export_bytes)
slab_bytes=$(kv vram_slab_bytes)
obj_bytes=$(kv vram_obj_bytes)
lz4=$(kv pages_lz4)
raw=$(kv pages_raw)
same=$(kv pages_same)
enospc=$(kv enospc)

cfg=${cfg:-0}
cfg10=${cfg10:-0}
vram_bytes=${vram_bytes:-0}
export_bytes=${export_bytes:-0}
slab_bytes=${slab_bytes:-0}
obj_bytes=${obj_bytes:-0}
lz4=${lz4:-0}
raw=${raw:-0}
same=${same:-0}
enospc=${enospc:-0}

pages=$(( lz4 + raw + same ))
stored_bytes=$(( pages * 4096 ))
payload=$(( lz4 + raw ))

vram_mib=$(mib "$vram_bytes")
export_mib=$(mib "$export_bytes")
stored_mib=$(mib "$stored_bytes")
slab_mib=$(mib "$slab_bytes")
obj_mib=$(mib "$obj_bytes")

pool_pct=0
logical_pct=0
[ "$vram_bytes" -gt 0 ] && pool_pct=$(( slab_bytes * 100 / vram_bytes ))
[ "$export_bytes" -gt 0 ] && logical_pct=$(( stored_bytes * 100 / export_bytes ))

# Effective ratio uses slab occupancy (what actually fills VRAM), not object
# payload. Same-filled pages take no VRAM and correctly raise this number.
eff="n/a"
if [ "$slab_bytes" -gt 0 ]; then
    eff=$(awk -v s="$stored_bytes" -v v="$slab_bytes" 'BEGIN { printf "%.2f", s / v }')
elif [ "$pages" -gt 0 ]; then
    eff="inf"
fi

echo "nbd-vram compression"
echo ""
echo "  configured ratio : ${cfg}x   (${export_mib} MiB swap on ${vram_mib} MiB VRAM)"
if [ "$eff" = "inf" ]; then
    echo "  effective ratio  : infinite   (${stored_mib} MiB stored, 0 MiB VRAM used)"
else
    echo "  effective ratio  : ${eff}x   (${stored_mib} MiB stored in ${slab_mib} MiB VRAM)"
fi
echo "  VRAM pool        : ${slab_mib} / ${vram_mib} MiB  (${pool_pct}% full)"
echo "  packed objects   : ${obj_mib} MiB  (size-class occupancy inside those slabs)"
echo "  swap device      : ${stored_mib} / ${export_mib} MiB stored  (${logical_pct}% of advertised size)"
echo "  pages            : ${pages}  (lz4 ${lz4}, uncompressed ${raw}, same-filled ${same})"
echo "  ENOSPC writes    : ${enospc}"
echo ""

if [ "$pages" -lt 256 ]; then
    echo "  Hint: not enough swapped data yet to judge. Put the machine under"
    echo "        memory pressure, then run nbd-vram-compression-status.sh again."
elif [ "$enospc" -gt 0 ]; then
    echo "  Hint: the VRAM pool filled before the advertised swap device did."
    echo "        Data is not compressing enough for ratio ${cfg}x - keep or lower"
    echo "        VRAM_COMPRESS_RATIO. Do not increase it."
elif [ "$payload" -lt 256 ] && [ "$same" -gt "$payload" ]; then
    echo "  Hint: almost everything stored is same-filled (zeros), which takes no"
    echo "        VRAM and inflates the ratio. Wait for real swap traffic before"
    echo "        raising VRAM_COMPRESS_RATIO."
elif [ "$slab_bytes" -gt 0 ]; then
    # Compare effective vs configured in hundredths
    eff100=$(awk -v s="$stored_bytes" -v v="$slab_bytes" 'BEGIN { printf "%d", (s * 100) / v }')
    cfg100=$(( cfg10 * 10 ))
    next10=$(( cfg10 + 10 ))
    if [ "$next10" -gt 80 ]; then next10=80; fi
    next="$(awk -v t="$next10" 'BEGIN { printf "%.1f", t / 10.0 }')"
    if [ "$eff100" -ge $(( cfg100 + 50 )) ] && [ "$pool_pct" -lt 70 ] && [ "$cfg10" -lt 80 ]; then
        echo "  Hint: effective ${eff}x is above configured ${cfg}x and the pool is"
        echo "        only ${pool_pct}% full. You can try VRAM_COMPRESS_RATIO=${next}."
    elif [ "$pool_pct" -ge 85 ] && [ "$logical_pct" -lt 70 ]; then
        echo "  Hint: VRAM is ${pool_pct}% full with only ${logical_pct}% of the swap"
        echo "        device used. Do not increase VRAM_COMPRESS_RATIO."
    else
        echo "  Hint: effective ${eff}x is close to configured ${cfg}x. Leave it."
    fi
else
    echo "  Hint: nothing is using VRAM yet (only empty or same-filled pages)."
fi
