#!/bin/bash
#
# measure_fragmentation.sh — report fragmentation of the evfs benchmark image.
#
# Prints:
#   - avg extents per file (file fragmentation score)
#   - % of files with more than 1 extent
#   - e2freefrag free-space histogram
#
# Usage: sudo ./test/filebench/measure_fragmentation.sh
#
# The image must already be mounted at evfs-sandbox-20gb/ (/mnt/fb).
# Run as root (filefrag needs read access to all files).

set -e

REPO=$(realpath "$(dirname "$0")/../..")
MOUNTPT="$REPO/evfs-sandbox-20gb"
IMG="$REPO/evfs-sandbox-20gb.img"

if ! mountpoint -q "$MOUNTPT"; then
    echo "ERROR: $MOUNTPT is not mounted. Run setup_img.sh first." >&2
    exit 1
fi

echo "=== File fragmentation (filefrag) ==="
find "$MOUNTPT" -type f -print0 \
    | xargs -0 filefrag 2>/dev/null \
    | awk '
        /extents? found/ {
            extents = $2
            sum += extents
            n++
            if (extents > 1) fragmented++
        }
        END {
            printf "Total files:          %d\n", n
            printf "Avg extents/file:     %.2f\n", (n > 0 ? sum/n : 0)
            printf "Files with >1 extent: %d (%.1f%%)\n", fragmented, (n > 0 ? 100.0*fragmented/n : 0)
        }
    '

echo ""
echo "=== Free-space fragmentation (e2freefrag) ==="
e2freefrag "$IMG" 2>/dev/null || echo "(unmount image first for e2freefrag, or ignore this)"
