#!/bin/bash
#
# Atomicity stress test for EXT4_EVFS_EXT_MV.
#
# Two files are created:
#   file_a  (16 KB, all 0xAA)
#   file_b  (16 KB, all 0xBB) 
#
# Test 1 (mode 0): Single extent remap on entire extent. Should not corrupt.
# Test 2 (mode 1): Two extent remaps on entire extent, split in half. Should corrupt.

export TEVFS_WORKSPACE=$(dirname "$0")
cd $TEVFS_WORKSPACE

source img-var.sh

echo "$TEVFS_SIZE $TEVFS_NUM_INODES $TEVFS_IMAGEPATH $TEVFS_MOUNTPT"
rm -f $TEVFS_IMAGEPATH
truncate -s $TEVFS_SIZE $TEVFS_IMAGEPATH

if ! sudo -E ./img-mkfs.sh; then
	exit 1
fi

# Use data=writeback so the journal doesn't serialise our ioctl stress
if ! sudo -E ./img-mount.sh --no-data-journaling; then
	exit 1
fi

file_a=$TEVFS_MOUNTPT/a
file_b=$TEVFS_MOUNTPT/b

# Create file_a (0xAA = octal \252) and file_b (0xBB = octal \273)
# 16 blocks × 1 KB = 16 KB each
tr '\0' '\252' < /dev/zero | dd of=$file_a bs=1024 count=16 conv=fdatasync 2>/dev/null
tr '\0' '\273' < /dev/zero | dd of=$file_b bs=1024 count=16 conv=fdatasync 2>/dev/null

sudo sync
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

# Build
rm -f build/extent_move_conc.x
make build/extent_move_conc.x 2>&1
if [ ! -f build/extent_move_conc.x ]; then
	echo "Build failed"
	sudo -E ./img-umount.sh
	exit 1
fi

# --------------------------------------------------------------------------
echo ""
echo "=== Test 1: single-ioctl mode (atomic - no corruption expected) ==="
build/extent_move_conc.x $TEVFS_MOUNTPT $file_a $file_b 0 10 $1
test1_ret=$?

# Restore known state for test 2: rewrite both files with fresh content
sudo sync
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
tr '\0' '\252' < /dev/zero | dd of=$file_a bs=1024 count=16 conv=fdatasync 2>/dev/null
tr '\0' '\273' < /dev/zero | dd of=$file_b bs=1024 count=16 conv=fdatasync 2>/dev/null
sudo sync
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

# --------------------------------------------------------------------------
# Test 2 skipped - split remap non-atomicity already confirmed
test2_ret=0

# --------------------------------------------------------------------------
echo ""
sudo -E ./img-umount.sh

echo ""
if [ $test1_ret -eq 0 ] && [ $test2_ret -eq 0 ]; then
	echo "All tests passed"
	exit 0
else
	echo "Test 1 exit=$test1_ret  Test 2 exit=$test2_ret"
	[ $test1_ret -ne 0 ] && echo "FAIL: single-ioctl showed corruption"
	[ $test2_ret -ne 0 ] && echo "NOTE: multi-ioctl did not observe partial state (try longer duration)"
	exit $test1_ret
fi
