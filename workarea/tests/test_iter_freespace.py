#!/usr/bin/env python3
"""
Tests for EXT4_IOC_ITER_FREESPACE ioctl.
Run from ~/code: sudo python3 workarea/test_iter_freespace.py
"""

import os
import fcntl
import struct
import sys
import subprocess

# _IOWR('f', 106, struct{ uint64, uint64, uint64 }) — 24 bytes, direction = 2
EXT4_IOC_ITER_FREESPACE = (2 << 30) | (24 << 16) | (0x66 << 8) | 106

# _IOR('f', 107, struct{ uint64, uint64*, uint32, uint32 }) 
# We need this to verify file blocks don't appear in free space
# Adjust number to match your header
EXT4_IOC_GET_INODE_EXTENTS = (2 << 30) | (24 << 16) | (0x66 << 8) | 107

SANDBOX = "/home/evie/code/evfs-sandbox"
TEST_DIR = os.path.join(SANDBOX, "test_iter_freespace")

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"

passed = 0
failed = 0

def iter_freespace(fd, start):
    """Returns (result_block, result_length). Both 0 means no free extent found."""
    buf = struct.pack("QQQ", start, 0, 0)
    result = fcntl.ioctl(fd, EXT4_IOC_ITER_FREESPACE, buf)
    _, result_block, result_length = struct.unpack("QQQ", result)
    return result_block, result_length

def check(name, condition, detail=""):
    global passed, failed
    if condition:
        print(f"  [{PASS}] {name}")
        passed += 1
    else:
        print(f"  [{FAIL}] {name}" + (f": {detail}" if detail else ""))
        failed += 1

def create_file(path, size_bytes=4096):
    """Create a file of given size and return its inode number."""
    with open(path, "wb") as f:
        f.write(b"x" * size_bytes)
    os.sync()  # flush to disk so bitmap reflects reality
    return os.stat(path).st_ino

def get_block_device():
    """Find the block device evfs-sandbox is mounted on."""
    result = subprocess.check_output(["df", SANDBOX]).decode()
    return result.split("\n")[1].split()[0]

def get_block_size(device):
    result = subprocess.check_output(["tune2fs", "-l", device]).decode()
    for line in result.split("\n"):
        if "Block size" in line:
            return int(line.split(":")[1].strip())
    return 4096

def read_block(device, block_num, block_size):
    """Read raw bytes at a given block number from the device."""
    result = subprocess.run(
        ["dd", f"if={device}", f"bs={block_size}",
         f"skip={block_num}", "count=1", "status=none"],
        capture_output=True
    )
    return result.stdout

def collect_all_free_extents(fd, max_extents=10000):
    """Chain iter_freespace calls to collect all free extents."""
    extents = []
    cursor = 0
    for _ in range(max_extents):
        block, length = iter_freespace(fd, cursor)
        if block == 0 and length == 0:
            break
        extents.append((block, length))
        cursor = block + length
    return extents

# ------------------------------------------------------------------ #

def test_result_strictly_after_start(fd):
    """result_block must always be strictly greater than start_block."""
    print("\ntest_result_strictly_after_start")
    block, length = iter_freespace(fd, 0)
    if block == 0 and length == 0:
        print("  [SKIP] filesystem has no free space")
        return
    check("result_block > start_block", block > 0,
          f"start=0, got result_block={block}")

    # also check mid-filesystem
    block2, length2 = iter_freespace(fd, block + length)
    if block2 != 0:
        check("result_block > start for mid-filesystem call",
              block2 > block + length,
              f"start={block+length}, got {block2}")

def test_result_length_at_least_one(fd):
    """Any returned free extent must have length >= 1."""
    print("\ntest_result_length_at_least_one")
    block, length = iter_freespace(fd, 0)
    if block == 0 and length == 0:
        print("  [SKIP] filesystem has no free space")
        return
    check("length >= 1", length >= 1, f"got length={length}")

def test_past_end_returns_zero(fd):
    """Starting past the last block should return (0, 0)."""
    print("\ntest_past_end_returns_zero")
    block, length = iter_freespace(fd, 2**64 - 1)
    check("returns (0,0) past end", block == 0 and length == 0,
          f"got ({block}, {length})")

def test_chained_extents_do_not_overlap(fd):
    """Free extents returned by chaining should never overlap."""
    print("\ntest_chained_extents_do_not_overlap")
    extents = collect_all_free_extents(fd)
    if len(extents) < 2:
        print("  [SKIP] fewer than 2 free extents found")
        return

    overlaps = False
    for i in range(len(extents) - 1):
        end_i = extents[i][0] + extents[i][1]
        start_next = extents[i+1][0]
        if end_i > start_next:
            overlaps = True
            check("no overlap", False,
                  f"extent {extents[i]} overlaps with {extents[i+1]}")
            break

    if not overlaps:
        check("no overlapping extents", True)

def test_chained_extents_are_ascending(fd):
    """Free extents returned by chaining should be in ascending block order."""
    print("\ntest_chained_extents_are_ascending")
    extents = collect_all_free_extents(fd)
    if len(extents) < 2:
        print("  [SKIP] fewer than 2 free extents found")
        return

    starts = [e[0] for e in extents]
    check("extents are ascending", starts == sorted(starts),
          f"got out-of-order extents: {extents[:5]}")

def test_used_blocks_not_in_free_extents(fd):
    """Blocks occupied by a live file should not appear in any free extent."""
    print("\ntest_used_blocks_not_in_free_extents")
    path = os.path.join(TEST_DIR, "used_block_file.txt")
    # Write enough data to guarantee at least one full block is allocated
    with open(path, "wb") as f:
        f.write(b"u" * 4096)
    os.sync()

    # Get the file's block via debugfs rather than the ioctl
    # to keep this test independent of get_inode_extents correctness
    ino = os.stat(path).st_ino
    device = get_block_device()
    result = subprocess.check_output(
        ["debugfs", "-R", f"stat <{ino}>", device],
        stderr=subprocess.DEVNULL
    ).decode()

    # Parse EXTENTS line from debugfs output
    file_blocks = set()
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
                file_blocks.add(int(phys))

    if not file_blocks:
        print("  [SKIP] could not parse file blocks from debugfs")
        return

    free_extents = collect_all_free_extents(fd)
    free_blocks = set()
    for start, length in free_extents:
        free_blocks.update(range(start, start + length))

    overlap = file_blocks & free_blocks
    check("live file blocks not in free extents", len(overlap) == 0,
          f"blocks {overlap} appear in both")

def test_deleted_file_blocks_become_free(fd):
    """After deleting a file and syncing, its blocks should appear as free."""
    print("\ntest_deleted_file_blocks_become_free")
    path = os.path.join(TEST_DIR, "to_delete.txt")
    with open(path, "wb") as f:
        f.write(b"d" * 4096)
    os.sync()

    ino = os.stat(path).st_ino
    device = get_block_device()

    # get blocks before deletion
    result = subprocess.check_output(
        ["debugfs", "-R", f"stat <{ino}>", device],
        stderr=subprocess.DEVNULL
    ).decode()

    file_blocks = set()
    in_extents = False
    for line in result.split("\n"):
        if "EXTENTS" in line.upper():
            in_extents = True
            continue
        if not in_extents:
            continue
        # extents lines look like: (0-11):34816 or (0):34816
        line = line.strip()
        if not line:
            break
        # physical block is after the colon
        if ":" in line:
            phys = line.split(":")[-1].strip()
            if phys.isdigit():
                file_blocks.add(int(phys))

    if not file_blocks:
        print("  [SKIP] could not parse file blocks from debugfs")
        return

    os.unlink(path)
    os.sync()
    # drop caches to force bitmap re-read
    with open("/proc/sys/vm/drop_caches", "w") as f:
        f.write("3\n")

    free_extents = collect_all_free_extents(fd)
    free_blocks = set()
    for start, length in free_extents:
        free_blocks.update(range(start, start + length))

    check("deleted file blocks now appear free",
          file_blocks.issubset(free_blocks),
          f"blocks {file_blocks - free_blocks} still not free")

def test_free_block_reads_as_zero_or_garbage(fd):
    """
    A block reported as free should not contain live file data.
    We write a known pattern to a file, delete it, then verify
    the reported free block no longer contains that pattern in a
    meaningful way — OR we verify a free block does not match
    any live file's content.
    """
    print("\ntest_reported_free_block_is_not_live_data")
    # Create a file with a unique pattern
    path = os.path.join(TEST_DIR, "pattern_file.txt")
    pattern = b"EVFS_TEST_UNIQUE_PATTERN_XYZ" * 100
    with open(path, "wb") as f:
        f.write(pattern)
    os.sync()

    # Read some free blocks and verify none contain our pattern
    device = get_block_device()
    block_size = get_block_size(device)

    block, length = iter_freespace(fd, 0)
    if block == 0:
        print("  [SKIP] no free extents found")
        return

    data = read_block(device, block, block_size)
    check("free block does not contain live file pattern",
          pattern[:28] not in data,
          f"found live pattern in supposedly free block {block}")

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
        test_result_strictly_after_start(fd)
        test_result_length_at_least_one(fd)
        # test_past_end_returns_zero(fd)
        test_chained_extents_do_not_overlap(fd)
        test_chained_extents_are_ascending(fd)
        test_used_blocks_not_in_free_extents(fd)
        test_deleted_file_blocks_become_free(fd)
        test_free_block_reads_as_zero_or_garbage(fd)
    finally:
        os.close(fd)
        cleanup()
        print(f"\n{'='*40}")
        print(f"Results: {passed} passed, {failed} failed")

if __name__ == "__main__":
    main()