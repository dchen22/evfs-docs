#!/bin/bash
#
# fragment.sh — age an ext4 filesystem image using Geriatrix.
#
# Usage: sudo ./test/filebench/fragment.sh <image_path> [profile] [seed]
#
#   image_path  path to the ext4 .img file to age in-place
#   profile     Geriatrix profile directory name (default: agrawal)
#               options: agrawal dabre douceur grundman meyer pramod wang_lanl wang_os
#   seed        RNG seed for reproducibility (default: 42)
#
# Mounts the image, runs Geriatrix to 70% utilisation using the chosen
# profile, then unmounts and reports fragmentation statistics.
#
# After running, snapshot the result before benchmarking:
#   cp evfs-sandbox-20gb.img evfs-aged-snap.img
#   # restore before each aged-scenario run:
#   cp evfs-aged-snap.img evfs-sandbox-20gb.img
#
# Run as root.

set -e

IMG=${1:?usage: fragment.sh <image_path> [profile] [seed]}
PROFILE=${2:-agrawal}
SEED=${3:-42}

GERIATRIX_PROFILES="/home/evie/geriatrix/profiles"

# ── locate geriatrix ──────────────────────────────────────────────────────────
GERIATRIX=$(command -v geriatrix 2>/dev/null || echo "/opt/bin/geriatrix")
if [ ! -x "$GERIATRIX" ]; then
    echo "ERROR: geriatrix not found at $GERIATRIX. Build it first:" >&2
    echo "  sudo apt-get install cmake libboost-all-dev" >&2
    echo "  git clone https://github.com/saurabhkadekodi/geriatrix ~/geriatrix" >&2
    echo "  cd ~/geriatrix && mkdir build && cd build" >&2
    echo "  cmake -DCMAKE_INSTALL_PREFIX=/opt .. && make && sudo make install" >&2
    exit 1
fi
echo "Using geriatrix: $GERIATRIX"

# ── locate profile files ──────────────────────────────────────────────────────
AGE_FILE="$GERIATRIX_PROFILES/$PROFILE/age_distribution.txt"
SIZE_FILE="$GERIATRIX_PROFILES/$PROFILE/size_distribution.txt"
DIR_FILE="$GERIATRIX_PROFILES/$PROFILE/dir_distribution.txt"

for f in "$AGE_FILE" "$SIZE_FILE" "$DIR_FILE"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: profile file not found: $f" >&2
        echo "Available profiles: $(ls $GERIATRIX_PROFILES)" >&2
        exit 1
    fi
done

# ── disk size (bytes) from image file ────────────────────────────────────────
DISK_BYTES=$(stat -c %s "$IMG")
echo "Image size: $DISK_BYTES bytes ($(( DISK_BYTES / 1024 / 1024 / 1024 )) GB)"

# ── mount ─────────────────────────────────────────────────────────────────────
MOUNTPT=$(mktemp -d /tmp/frag-XXXXXX)
cleanup() { umount "$MOUNTPT" 2>/dev/null || true; rmdir "$MOUNTPT" 2>/dev/null || true; }
trap cleanup EXIT

echo "=== Mounting $IMG at $MOUNTPT ==="
mount -o loop,data=writeback "$IMG" "$MOUNTPT"

# ── pre-aging stats ───────────────────────────────────────────────────────────
echo ""
echo "=== Pre-aging free-space fragmentation ==="
e2freefrag "$IMG" 2>/dev/null || true

# ── run Geriatrix ─────────────────────────────────────────────────────────────
# -n  disk size in bytes
# -u  target utilisation (0.7 = 70% full); leaves headroom for filebench files
# -r  random seed
# -m  mount point (no trailing /)
# -a/s/d  distribution files for age, size, directory depth
# -x/y/z  output files for the resulting distributions
# -t  threads (1 = fully reproducible; >1 is faster but non-deterministic)
# -i  aging iterations (3 = 3× disk size worth of creates/deletes)
# -f 0  real mode (not fake/dry-run)
# -p 0  no idle time injection
# -c 0.9  stop at 90% convergence (faster than perfect convergence)
# -q 0  non-interactive (don't ask before quitting)
# -w 60  hard time limit: 60 minutes
# -b posix  POSIX backend
echo ""
echo "=== Running Geriatrix (profile=$PROFILE seed=$SEED util=70% max=60min) ==="
"$GERIATRIX" \
    -n "$DISK_BYTES" \
    -u 0.2 \
    -r "$SEED" \
    -m "$MOUNTPT" \
    -a "$AGE_FILE" \
    -s "$SIZE_FILE" \
    -d "$DIR_FILE" \
    -x /tmp/geriatrix_age.out \
    -y /tmp/geriatrix_size.out \
    -z /tmp/geriatrix_dir.out \
    -t 1 \
    -i 3 \
    -f 0 \
    -p 0 \
    -c 0.9 \
    -q 0 \
    -w 2 \
    -b posix

sync

# ── post-aging stats ──────────────────────────────────────────────────────────
echo ""
echo "=== Post-aging free-space fragmentation ==="
e2freefrag "$IMG" 2>/dev/null || true

echo ""
echo "=== Sample file extent counts ==="
find "$MOUNTPT" -type f 2>/dev/null | shuf | head -10 | xargs filefrag 2>/dev/null || true

echo ""
echo "=== Aging complete. Image: $IMG ==="
echo "Snapshot it before benchmarking: cp $IMG <snapshot>.img"
