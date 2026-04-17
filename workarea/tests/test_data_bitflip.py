#!/usr/bin/env python3
"""
Tests for EXT4_IOC_FLIP_BLOCK_BIT ioctl.
Run from ~/code: sudo python3 workarea/tests/test_flip_block_bit.py
"""

import os
import fcntl
import struct
import sys
import subprocess

# _IOW('f', 100, uint64) — 8 bytes, direction = 1 (write)
EXT4_IOC_FLIP_BLOCK_BIT  = (1 << 30) | (8 << 16) | (0x66 << 8) | 100
# _IOR('f', 106, struct{ uint64, uint64, uint64 }) — 24 bytes, direction = 2
EXT4_IOC_ITER_FREESPACE  = (2 << 30) | (24 << 16) | (0x66 << 8) | 106

SANDBOX = "/home/evie/code/evfs-sandbox"
TEST_DIR = os.path.join(SANDBOX, "test_flip_block_bit")

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"

passed = 0
failed = 0

def flip_block_bit(fd, block_number):
    """Call EXT4_IOC_FLIP_BLOCK_BIT on a given block number."""
    buf = struct.pack("Q", block_number)
    fcntl.ioctl(fd, EXT4_IOC_FLIP_BLOCK_BIT, buf)

def iter_freespace(fd, start):
    """Returns (result_block, result_length). Both 0 means no free extent found."""
    buf = struct.pack("QQQ", start, 0, 0)
    result = fcntl.ioctl(fd, EXT4_IOC_ITER_FREESPACE, buf)
    _, result_block, result_length = struct.unpack("QQQ", result)
    return result_block, result_length

def collect_all_free_extents(fd, max_extents=10000):
    extents = []
    cursor = 0
    for _ in range(max_extents):
        block, length = iter_freespace(fd, cursor)
        if block == 0 and length == 0:
            break
        extents.append((block, length))
        cursor = block + length
    return extents

def get_free_blocks(fd):
    free_blocks = set()
    for start, length in collect_all_free_extents(fd):
        free_blocks.update(range(start, start + length))
    return free_blocks

def get_block_device():
    result = subprocess.check_output(["df", SANDBOX]).decode()
    return result.split("\n")[1].split()[0]

def get_block_size(device):
    result = subprocess.check_output(["tune2fs", "-l", device]).decode()
    for line in result.split("\n"):
        if "Block size" in line:
            return int(line.split(":")[1].strip())
    return 4096

def read_block(device, block_num, block_size):
    result = subprocess.run(
        ["dd", f"if={device}", f"bs={block_size}",
         f"skip={block_num}", "count=1", "status=none"],
        capture_output=True
    )
    return result.stdout

def drop_caches():
    with open("/proc/sys/vm/drop_caches", "w") as f:
        f.write("3\n")

def check(name, condition, detail=""):
    global passed, failed
    if condition:
        print(f"  [{PASS}] {name}")
        passed += 1
    else:
        print(f"  [{FAIL}] {name}" + (f": {detail}" if detail else ""))
        failed += 1

def find_free_block(fd):
    """Find a free block to use for testing."""
    block, length = iter_freespace(fd, 0)
    if block == 0 and length == 0:
        return None
    return block

# ------------------------------------------------------------------ #

def test_flip_free_block_marks_used(fd):
    """Flipping a free block's bit should make it appear as used (no longer free)."""
    print("\ntest_flip_free_block_marks_used")
    block = find_free_block(fd)
    if block is None:
        print("  [SKIP] no free blocks found")
        return

    flip_block_bit(fd, block)
    drop_caches()

    free_blocks = get_free_blocks(fd)
    check("block no longer appears free after flip",
          block not in free_blocks,
          f"block {block} still appears free")

    # restore
    flip_block_bit(fd, block)

def test_flip_twice_restores_original_state(fd):
    """Flipping a block twice should return it to its original state."""
    print("\ntest_flip_twice_restores_original_state")
    block = find_free_block(fd)
    if block is None:
        print("  [SKIP] no free blocks found")
        return

    # flip once — should be used
    flip_block_bit(fd, block)
    drop_caches()
    free_blocks_after_first = get_free_blocks(fd)
    check("block is used after first flip",
          block not in free_blocks_after_first,
          f"block {block} still free after first flip")

    # flip again — should be free again
    flip_block_bit(fd, block)
    drop_caches()
    free_blocks_after_second = get_free_blocks(fd)
    check("block is free again after second flip",
          block in free_blocks_after_second,
          f"block {block} not free after second flip")

def test_flip_used_block_marks_free(fd):
    """Flipping a used block's bit should make it appear as free."""
    print("\ntest_flip_used_block_marks_free")
    device = get_block_device()

    # create a file and find its block via debugfs
    path = os.path.join(TEST_DIR, "used_file.txt")
    with open(path, "wb") as f:
        f.write(b"u" * 4096)
    os.sync()

    ino = os.stat(path).st_ino
    result = subprocess.check_output(
        ["debugfs", "-R", f"stat <{ino}>", device],
        stderr=subprocess.DEVNULL
    ).decode()

    file_block = None
    in_extents = False
    for line in result.split("\n"):
        if "EXTENTS" in line.upper():
            in_extents = True
            continue
        if not in_extents:
            continue
        line = line.strip()
        if not line:
            break
        if ":" in line:
            phys = line.split(":")[-1].strip()
            if phys.isdigit():
                file_block = int(phys)
                break

    if file_block is None:
        print("  [SKIP] could not find file block via debugfs")
        return

    # verify block is not free before flip
    free_blocks_before = get_free_blocks(fd)
    check("file block is not free before flip",
          file_block not in free_blocks_before,
          f"block {file_block} already appears free")

    # flip — should now appear free
    flip_block_bit(fd, file_block)
    drop_caches()
    free_blocks_after = get_free_blocks(fd)
    check("file block appears free after flip",
          file_block in free_blocks_after,
          f"block {file_block} still not free after flip")

    # restore — flip back so filesystem stays consistent
    flip_block_bit(fd, file_block)

def test_flip_does_not_affect_adjacent_blocks(fd):
    """Flipping one block should not change the state of adjacent blocks."""
    print("\ntest_flip_does_not_affect_adjacent_blocks")
    block, length = iter_freespace(fd, 0)
    if block == 0 or length < 3:
        print("  [SKIP] need a free extent of at least 3 blocks")
        return

    # use the middle block so we have neighbours on both sides
    target = block + 1
    left   = block
    right  = block + 2

    free_blocks_before = get_free_blocks(fd)
    check("left neighbour is free before flip",
          left in free_blocks_before,
          f"block {left} not free before flip")
    check("right neighbour is free before flip",
          right in free_blocks_before,
          f"block {right} not free before flip")

    flip_block_bit(fd, target)
    drop_caches()

    free_blocks_after = get_free_blocks(fd)
    check("target block is used after flip",
          target not in free_blocks_after,
          f"block {target} still free")
    check("left neighbour unaffected",
          left in free_blocks_after,
          f"block {left} changed state unexpectedly")
    check("right neighbour unaffected",
          right in free_blocks_after,
          f"block {right} changed state unexpectedly")

    # restore
    flip_block_bit(fd, target)

def test_flip_updates_free_block_count(fd):
    """
    Flipping a free block should decrease the reported free block count,
    and flipping it back should restore it.
    """
    print("\ntest_flip_updates_free_block_count")
    device = get_block_device()

    def get_free_count():
        # force flush of superblock percpu free block counter to disk
        subprocess.check_call(["fsfreeze", "--freeze", SANDBOX])
        subprocess.check_call(["fsfreeze", "--unfreeze", SANDBOX])
        
        result = subprocess.check_output(
            ["tune2fs", "-l", device],
            stderr=subprocess.DEVNULL
        ).decode()
        for line in result.split("\n"):
            if "Free blocks" in line:
                return int(line.split(":")[1].strip())
        return None

    block = find_free_block(fd)
    if block is None:
        print("  [SKIP] no free blocks found")
        return

    count_before = get_free_count()
    if count_before is None:
        print("  [SKIP] could not read free block count")
        return

    flip_block_bit(fd, block)
    os.sync()
    count_after = get_free_count()

    check("free block count decreases by 1 after flipping free block",
          count_after == count_before - 1,
          f"before={count_before}, after={count_after}")

    flip_block_bit(fd, block)
    os.sync()
    count_restored = get_free_count()

    check("free block count restored after flipping back",
          count_restored == count_before,
          f"expected {count_before}, got {count_restored}")

def test_block_content_readable_after_flip(fd):
    """
    After flipping a free block to used, we should be able to
    read its contents from the block device without error.
    """
    print("\ntest_block_content_readable_after_flip")
    device = get_block_device()
    block_size = get_block_size(device)

    block = find_free_block(fd)
    if block is None:
        print("  [SKIP] no free blocks found")
        return

    flip_block_bit(fd, block)

    data = read_block(device, block, block_size)
    check("block is readable after flip",
          len(data) == block_size,
          f"expected {block_size} bytes, got {len(data)}")

    # restore
    flip_block_bit(fd, block)

# ------------------------------------------------------------------ #

def cleanup():
    if os.path.exists(TEST_DIR):
        for f in os.listdir(TEST_DIR):
            try:
                os.unlink(os.path.join(TEST_DIR, f))
            except:
                pass
        os.rmdir(TEST_DIR)

def main():
    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)

    os.makedirs(TEST_DIR, exist_ok=True)
    fd = os.open(SANDBOX, os.O_RDONLY)

    try:
        test_flip_free_block_marks_used(fd)
        test_flip_twice_restores_original_state(fd)
        test_flip_used_block_marks_free(fd)
        test_flip_does_not_affect_adjacent_blocks(fd)
        test_flip_updates_free_block_count(fd)
        test_block_content_readable_after_flip(fd)
    finally:
        os.close(fd)
        cleanup()
        print(f"\n{'='*40}")
        print(f"Results: {passed} passed, {failed} failed")

if __name__ == "__main__":
    main()