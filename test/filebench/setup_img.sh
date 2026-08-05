#!/bin/bash
#
# setup_img.sh — create or reimage the evfs-sandbox-20gb ext4 image.
#
# Creates a 20 GB ext4 filesystem image with 1 KB blocks, 2 M inodes, and
# data=writeback journalling, then mounts it at evfs-sandbox-20gb/ and
# symlinks /mnt/fb -> evfs-sandbox-20gb/ for filebench.
#
# If the image already exists it is unmounted, wiped, and reformatted.
# Safe to run multiple times.
#
# Usage: sudo ./test/filebench/setup_img.sh

set -e

if (( EUID != 0 )); then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

REPO=$(realpath "$(dirname "$0")/../..")
IMG="$REPO/evfs-sandbox-20gb.img"
MOUNTPT="$REPO/evfs-sandbox-20gb"
SYMLINK="/mnt/fb"

# ── unmount if currently mounted ──────────────────────────────────────────────
if mountpoint -q "$MOUNTPT" 2>/dev/null; then
    echo "=== Unmounting $MOUNTPT ==="
    umount "$MOUNTPT"
fi

# ── create image file (truncate is instant; no dd needed) ────────────────────
if [ -f "$IMG" ]; then
    echo "=== Reimaging existing $IMG ==="
else
    echo "=== Creating new $IMG (20 GB) ==="
    truncate -s 20G "$IMG"
fi

# ── format ────────────────────────────────────────────────────────────────────
echo "=== Formatting as ext4 (1 KB blocks, 2 M inodes) ==="
mkfs.ext4 -b 1024 -N 2000000 -F "$IMG"

# ── mount point ───────────────────────────────────────────────────────────────
mkdir -p "$MOUNTPT"
echo "=== Mounting at $MOUNTPT (data=writeback) ==="
mount -o loop,data=writeback "$IMG" "$MOUNTPT"
chown "${SUDO_USER:-root}:${SUDO_USER:-root}" "$MOUNTPT"

# ── /mnt/fb symlink (required by filebench workload) ─────────────────────────
if [ -L "$SYMLINK" ] || [ -e "$SYMLINK" ]; then
    rm -f "$SYMLINK"
fi
ln -s "$MOUNTPT" "$SYMLINK"
echo "=== Symlinked $SYMLINK -> $MOUNTPT ==="

# permissions
chmod 777 $IMG

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
df -h "$MOUNTPT"
echo ""
echo "=== Done. Image ready at $IMG, mounted at $MOUNTPT ==="
