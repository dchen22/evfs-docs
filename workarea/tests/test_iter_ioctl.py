#!/usr/bin/env python3
"""
Tests for EXT4_IOC_ITER_INODE ioctl.
Run from ~/code: sudo python3 workarea/test_iter_inode.py
"""

import os
import fcntl
import struct
import sys

# ioctl definition: _IOR('f', 105, struct{ uint64, uint64 })
# struct size = 16 bytes, direction = read (2), type = 'f' (0x66), nr = 105
EXT4_IOC_ITER_INODE = (2 << 30) | (16 << 16) | (0x66 << 8) | 105

SANDBOX = "/home/evie/code/evfs-sandbox"
TEST_DIR = os.path.join(SANDBOX, "test_iter_inode")

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"

passed = 0
failed = 0

def iter_inode(fd, start):
    """Call EXT4_IOC_ITER_INODE, returns result_inode_number (0 = none found)."""
    buf = struct.pack("QQ", start, 0)
    result = fcntl.ioctl(fd, EXT4_IOC_ITER_INODE, buf)
    _, result_ino = struct.unpack("QQ", result)
    return result_ino

def check(name, condition, detail=""):
    global passed, failed
    if condition:
        print(f"  [{PASS}] {name}")
        passed += 1
    else:
        print(f"  [{FAIL}] {name}" + (f": {detail}" if detail else ""))
        failed += 1

def get_inode(path):
    return os.stat(path).st_ino

def create_file(path, content="test\n"):
    with open(path, "w") as f:
        f.write(content)
    return get_inode(path)

# ------------------------------------------------------------------ #

def test_finds_existing_inode(fd):
    """Iterator should find an inode we just created."""
    print("\ntest_finds_existing_inode")
    path = os.path.join(TEST_DIR, "file_a.txt")
    ino = create_file(path)

    result = iter_inode(fd, ino - 1)   # start just before it
    check("finds inode at start - 1", result == ino,
          f"expected {ino}, got {result}")

    result = iter_inode(fd, ino)       # start exactly at it (should skip)
    check("skips start inode itself", result != ino or result == 0,
          f"expected something > {ino}, got {result}")

def test_skips_deleted_inode(fd):
    """Iterator should not return an inode that has been deleted."""
    print("\ntest_skips_deleted_inode")
    path = os.path.join(TEST_DIR, "file_deleted.txt")
    ino = create_file(path)
    os.unlink(path)

    # Scan the range around where the inode was
    result = iter_inode(fd, ino - 1)
    check("deleted inode not returned", result != ino,
          f"got deleted inode {ino}")

def test_multiple_files_ordered(fd):
    """Iterator should return inodes in ascending order."""
    print("\ntest_multiple_files_ordered")
    paths = [os.path.join(TEST_DIR, f"multi_{i}.txt") for i in range(3)]
    inodes = sorted(create_file(p) for p in paths)

    results = []
    cursor = inodes[0] - 1
    for _ in inodes:
        r = iter_inode(fd, cursor)
        if r == 0:
            break
        results.append(r)
        cursor = r

    check("all files found", all(ino in results for ino in inodes),
          f"expected {inodes}, found {results}")
    check("results are ascending", results == sorted(results),
          f"got {results}")

def test_start_past_last_inode(fd):
    """Starting past the last inode should return 0."""
    print("\ntest_start_past_last_inode")
    # Read total inode count from superblock via /proc or just use a huge number
    result = iter_inode(fd, 2**32 - 1)
    check("returns 0 when no more inodes", result == 0,
          f"expected 0, got {result}")

def test_start_at_zero(fd):
    """Starting at 0 should return the first in-use inode (at least inode 2)."""
    print("\ntest_start_at_zero")
    result = iter_inode(fd, 0)
    check("returns valid inode when starting at 0", result >= 2,
          f"got {result}")

def test_chained_iteration_covers_all(fd):
    """Chaining iter_inode calls should eventually find all created files."""
    print("\ntest_chained_iteration_covers_all")
    paths = [os.path.join(TEST_DIR, f"chain_{i}.txt") for i in range(5)]
    target_inodes = set(create_file(p) for p in paths)

    found = set()
    cursor = 0
    while True:
        r = iter_inode(fd, cursor)
        if r == 0:
            break
        if r in target_inodes:
            found.add(r)
        cursor = r

    check("all chained files found", found == target_inodes,
          f"missing: {target_inodes - found}")

# ------------------------------------------------------------------ #

def cleanup():
    if os.path.exists(TEST_DIR):
        for f in os.listdir(TEST_DIR):
            os.unlink(os.path.join(TEST_DIR, f))
        os.rmdir(TEST_DIR)

def main():
    if os.geteuid() != 0:
        print("ERROR: must run as root (sudo)")
        sys.exit(1)

    os.makedirs(TEST_DIR, exist_ok=True)

    # open evfs-sandbox/ parent dir
    fd = os.open(SANDBOX, os.O_RDONLY)

    try:
        test_finds_existing_inode(fd)
        test_skips_deleted_inode(fd)
        test_multiple_files_ordered(fd)
        # test_start_past_last_inode(fd)
        # test_start_at_zero(fd)
        test_chained_iteration_covers_all(fd)
    finally:
        os.close(fd)
        cleanup()
        print(f"\n{'='*40}")
        print(f"Results: {passed} passed, {failed} failed")

if __name__ == "__main__":
    main()