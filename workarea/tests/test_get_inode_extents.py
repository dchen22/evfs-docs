#!/usr/bin/env python3
"""
Tests for EXT4_IOC_GET_INODE_EXTENTS ioctl.
Run from ~/code: sudo python3 workarea/tests/test_get_inode_extents.py
"""

import os
import fcntl
import struct
import ctypes
import subprocess
import sys

SANDBOX = "/home/evie/code/evfs-sandbox"
TEST_DIR = os.path.join(SANDBOX, "test_get_inode_extents")

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"

passed = 0
failed = 0

# struct ext4_evfs_extent { uint64 start_block; uint32 length; }
# struct ext4_evfs_get_inode_extents {
#     uint64 inode_number;
#     uint32 max_num_extents;
#     uint32 result_num_extents;
#     uint64 extents_ptr;   <- userspace pointer
# }
# size = 8+4+4+8 = 24 bytes, _IOR = direction 2
EXT4_IOC_GET_INODE_EXTENTS = (2 << 30) | (24 << 16) | (0x66 << 8) | 107

EXTENT_SIZE = 12  # sizeof(ext4_evfs_extent) = 8 + 4

class Extent(ctypes.Structure):
    _fields_ = [
        ("start_block", ctypes.c_uint64),
        ("length",      ctypes.c_uint32),
    ]

def get_inode_extents(fd, inode_number, max_extents=128):
    """
    Call EXT4_IOC_GET_INODE_EXTENTS.
    Returns list of (start_block, length) tuples, or raises OSError.
    """
    ExtentArray = Extent * max_extents
    buf = ExtentArray()
    buf_ptr = ctypes.addressof(buf)

    # pack: inode_number(Q), max_num_extents(I), result_num_extents(I), ptr(Q)
    req = bytearray(struct.pack("QIIQ", inode_number, max_extents, 0, buf_ptr))
    result = fcntl.ioctl(fd, EXT4_IOC_GET_INODE_EXTENTS, req)
    _, _, result_num, _ = struct.unpack("QIIQ", req)

    return [(buf[i].start_block, buf[i].length) for i in range(result_num)]

def check(name, condition, detail=""):
    global passed, failed
    if condition:
        print(f"  [{PASS}] {name}")
        passed += 1
    else:
        print(f"  [{FAIL}] {name}" + (f": {detail}" if detail else ""))
        failed += 1

def get_block_device():
    result = subprocess.check_output(["df", SANDBOX]).decode()
    return result.split("\n")[1].split()[0]

def debugfs_extents(inode_number, device):
    """Use debugfs as independent ground truth for an inode's extents."""
    result = subprocess.check_output(
        ["debugfs", "-R", f"stat <{inode_number}>", device],
        stderr=subprocess.DEVNULL
    ).decode()

    extents = []
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
                extents.append(int(phys))
    return extents

def create_file(path, size_bytes):
    with open(path, "wb") as f:
        f.write(b"x" * size_bytes)
    os.sync()
    return os.stat(path).st_ino

# ------------------------------------------------------------------ #

def test_single_block_file_matches_debugfs(fd, device):
    """A small file's single extent should match what debugfs reports."""
    print("\ntest_single_block_file_matches_debugfs")
    path = os.path.join(TEST_DIR, "small.txt")
    ino = create_file(path, 4096)

    extents = get_inode_extents(fd, ino)
    db_blocks = debugfs_extents(ino, device)

    if not db_blocks:
        print("  [SKIP] debugfs could not parse extents")
        return

    check("returns at least one extent", len(extents) >= 1,
          f"got {extents}")
    if extents:
        check("start_block matches debugfs", extents[0][0] == db_blocks[0],
              f"ioctl={extents[0][0]}, debugfs={db_blocks[0]}")
        check("length is at least 1", extents[0][1] >= 1,
              f"got length={extents[0][1]}")

def test_large_file_covers_all_blocks(fd, device):
    """A multi-block file's extents should cover all blocks debugfs reports."""
    print("\ntest_large_file_covers_all_blocks")
    path = os.path.join(TEST_DIR, "large.txt")
    # write 10 blocks worth of data
    ino = create_file(path, 4096 * 10)

    extents = get_inode_extents(fd, ino)
    db_blocks = debugfs_extents(ino, device)

    if not db_blocks:
        print("  [SKIP] debugfs could not parse extents")
        return

    # expand ioctl extents into flat set of block numbers
    ioctl_blocks = set()
    for start, length in extents:
        ioctl_blocks.update(range(start, start + length))

    db_block_set = set(db_blocks)
    check("all debugfs blocks covered by ioctl extents",
          db_block_set.issubset(ioctl_blocks),
          f"missing: {db_block_set - ioctl_blocks}")

def test_extents_are_ascending(fd):
    """Extents should be returned in ascending block order."""
    print("\ntest_extents_are_ascending")
    path = os.path.join(TEST_DIR, "ascending.txt")
    ino = create_file(path, 4096 * 5)

    extents = get_inode_extents(fd, ino)
    if len(extents) < 2:
        print("  [SKIP] file has fewer than 2 extents")
        return

    starts = [e[0] for e in extents]
    check("extents are in ascending order", starts == sorted(starts),
          f"got {extents}")

def test_extents_do_not_overlap(fd):
    """No two extents returned for the same inode should overlap."""
    print("\ntest_extents_do_not_overlap")
    path = os.path.join(TEST_DIR, "nooverlap.txt")
    ino = create_file(path, 4096 * 5)

    extents = get_inode_extents(fd, ino)
    if len(extents) < 2:
        print("  [SKIP] file has fewer than 2 extents")
        return

    for i in range(len(extents) - 1):
        end_i = extents[i][0] + extents[i][1]
        start_next = extents[i+1][0]
        check(f"extent {i} does not overlap extent {i+1}",
              end_i <= start_next,
              f"{extents[i]} overlaps {extents[i+1]}")

def test_max_extents_zero_returns_zero(fd):
    """Passing max_num_extents=0 should return 0 extents."""
    print("\ntest_max_extents_zero_returns_zero")
    path = os.path.join(TEST_DIR, "zero_max.txt")
    ino = create_file(path, 4096)

    extents = get_inode_extents(fd, ino, max_extents=0)
    check("max_extents=0 returns empty list", len(extents) == 0,
          f"got {extents}")

def test_all_extents_have_nonzero_length(fd):
    """Every returned extent must have length >= 1."""
    print("\ntest_all_extents_have_nonzero_length")
    path = os.path.join(TEST_DIR, "nonzero_len.txt")
    ino = create_file(path, 4096 * 3)

    extents = get_inode_extents(fd, ino)
    check("got at least one extent", len(extents) >= 1)
    bad = [(s, l) for s, l in extents if l == 0]
    check("all extents have length >= 1", len(bad) == 0,
          f"zero-length extents: {bad}")

def test_all_extents_have_nonzero_start(fd):
    """Every returned extent must have start_block > 0 (block 0 is superblock)."""
    print("\ntest_all_extents_have_nonzero_start")
    path = os.path.join(TEST_DIR, "nonzero_start.txt")
    ino = create_file(path, 4096)

    extents = get_inode_extents(fd, ino)
    bad = [(s, l) for s, l in extents if s == 0]
    check("no extent starts at block 0", len(bad) == 0,
          f"extents starting at block 0: {bad}")

def test_invalid_inode_returns_error(fd):
    """Passing a non-existent inode number should return an error."""
    print("\ntest_invalid_inode_returns_error")
    try:
        extents = get_inode_extents(fd, 999999999)
        check("invalid inode returns error", False,
              f"expected OSError, got extents={extents}")
    except OSError:
        check("invalid inode returns error", True)

def test_deleted_inode_not_accessible(fd):
    """After deletion, the inode should no longer be accessible."""
    print("\ntest_deleted_inode_not_accessible")
    path = os.path.join(TEST_DIR, "to_delete.txt")
    ino = create_file(path, 4096)
    os.unlink(path)
    os.sync()

    try:
        extents = get_inode_extents(fd, ino)
        check("deleted inode returns error or empty", len(extents) == 0,
              f"got extents for deleted inode: {extents}")
    except OSError:
        check("deleted inode returns error or empty", True)

def test_extents_blocks_not_in_freespace(fd):
    """
    Blocks reported by get_inode_extents should not appear as
    free in iter_freespace — a live file's blocks are not free.
    """
    print("\ntest_extents_blocks_not_in_freespace")

    EXT4_IOC_ITER_FREESPACE = (2 << 30) | (24 << 16) | (0x66 << 8) | 106

    def iter_freespace(start):
        buf = struct.pack("QQQ", start, 0, 0)
        result = fcntl.ioctl(fd, EXT4_IOC_ITER_FREESPACE, buf)
        _, rb, rl = struct.unpack("QQQ", result)
        return rb, rl

    path = os.path.join(TEST_DIR, "cross_check.txt")
    ino = create_file(path, 4096)
    extents = get_inode_extents(fd, ino)

    if not extents:
        print("  [SKIP] no extents returned")
        return

    # collect all free blocks
    free_blocks = set()
    cursor = 0
    for _ in range(10000):
        rb, rl = iter_freespace(cursor)
        if rb == 0 and rl == 0:
            break
        free_blocks.update(range(rb, rb + rl))
        cursor = rb + rl

    file_blocks = set()
    for start, length in extents:
        file_blocks.update(range(start, start + length))

    overlap = file_blocks & free_blocks
    check("live file blocks not reported as free", len(overlap) == 0,
          f"blocks {overlap} appear in both")

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
    device = get_block_device()

    try:
        test_single_block_file_matches_debugfs(fd, device)
        test_large_file_covers_all_blocks(fd, device)
        test_extents_are_ascending(fd)
        test_extents_do_not_overlap(fd)
        test_max_extents_zero_returns_zero(fd)
        test_all_extents_have_nonzero_length(fd)
        test_all_extents_have_nonzero_start(fd)
        test_invalid_inode_returns_error(fd)
        test_deleted_inode_not_accessible(fd)
        test_extents_blocks_not_in_freespace(fd)
    finally:
        os.close(fd)
        cleanup()
        print(f"\n{'='*40}")
        print(f"Results: {passed} passed, {failed} failed")

if __name__ == "__main__":
    main()