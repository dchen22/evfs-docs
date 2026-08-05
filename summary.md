# evfs extent_move — test summary

## What is evfs?

evfs is a custom ioctl interface added to ext4 (`linux-6.8/fs/ext4/evfs.c`) that
exposes filesystem internals — block allocation, inode allocation, dentry
manipulation, extent tree reads and rewrites — directly to userspace.

## What does `EXT4_EVFS_EXT_MV` do?

Rewrites the physical block pointer(s) in an inode's extent tree. Caller
supplies `(ino_num, exp_iver, log_start, phy_start, len)`. The ioctl maps
logical blocks `[log_start, log_start+len)` to physical blocks
`[phy_start, phy_start+len)`. **Does not copy data** — only the extent tree
pointer changes.

### Atomicity and locking

One call is atomic over its entire `len`: the kernel holds `inode_lock` +
`down_write(i_data_sem)` for the full remap + journal commit. A concurrent
reader sees either the old or the new mapping, never a partial state. Two
separate calls (e.g. remapping two disjoint ranges) are **not** atomic with
respect to each other.

### `iversion` and `EAGAIN`

The ioctl takes `exp_iver`; if `i_version` has changed since the caller read
it, the kernel returns `-EAGAIN` (retry). In practice EAGAIN never fires
because neither reads nor `EXT_MV` itself increments `i_version` (there is a
`// TODO` in `evfs.c` to fix this).

## Concurrency / atomicity stress test (`extent_move_conc`)

Two CPU-pinned threads run for 10 s:

- **Remapper**: alternates `file_a`'s extent between physical blocks PA and PB
  via `EXT4_EVFS_EXT_MV`.
- **Verifier**: writes a rolling byte value, sleeps `delay_us` µs, then reads
  back with `O_DIRECT`. A mixed read (bytes from two different sources in one
  `pread`) is a torn read = atomicity violation.

Contention is measured via atomic "check-before-set" flags: each thread checks
whether the other is in its syscall before entering its own. This is a
**lower bound** — a nonzero count proves contention occurred; zero does not
prove it didn't.

### Results

| Mode | Remapper swaps | Corrupt reads | Contention events |
|---|---|---|---|
| Mode 0: single ioctl (atomic) | ~449k | **0** | ~102k |
| Mode 1: two ioctls (non-atomic) | ~134k | **57k (~53%)** | ~99k |

Mode 0: zero corruption under sustained real contention — atomicity confirmed.
Mode 1: ~50% corruption, every torn read splits exactly at the 8-block ioctl
boundary — non-atomicity of split remaps confirmed.

## Free-space iterator ioctl (`EXT4_EVFS_FSP_ITER`)

`evfs_fspace_iter` was already in the kernel but not wired to an ioctl. Added:
- `evfs.h`: `#define EXT4_EVFS_FSP_ITER _IOWR('f', 112, struct ext4_evfs_fsp_iter_args)`
- `evfs.c`: dispatch case copying args in/out and calling `evfs_fspace_iter`.

Returns one contiguous free run `(block, length)` per call. Has a TOCTOU race
with `BLK_ALLOC` (no atomic find-and-claim); callers retry on `-EEXIST`.

## Filebench macrobenchmark setup

**Goal**: measure whether continuous background extent remapping degrades
filebench webserver throughput (journal contention is the primary expected
mechanism — both workloads write to the same ext4 journal).

**Infrastructure:**
- 20 GB ext4 image (`evfs-sandbox-20gb.img`), mounted at `evfs-sandbox-20gb/`,
  symlinked to `/mnt/fb` for filebench's path length constraints.
- `test/filebench/webserver-bench.f`: 1M files, mean 16 KB (gamma), 100 threads,
  `reuse` flag so re-runs skip file recreation.
- `test/filebench/bench.sh`: Run 1 = filebench alone (baseline); Run 2 =
  filebench + background `remapper.x`; cache flushed between runs.
- `test/filebench/prerun.sh`: drops caches, disables ASLR, stops noisy services.
- `test/src/remapper.c`: swaps random pairs from a 500-file `remap_pool/` using
  `EXT4_EVFS_EXT_MV`. Includes diagnostic output for pool-load failures.

**Baseline result (Run 1):**
```
IO Summary: 1,359,918 ops  22,530 ops/s  7268/728 rd/wr  119.4 MB/s  3.944 ms/op
```

**Filebench build issues resolved:**

| Issue | Fix |
|---|---|
| `buffer overflow` crash (FORTIFY false positive on `char *dirs[65536]`) | Rebuild with `-D_FORTIFY_SOURCE=0 -fno-stack-protector`; added to `~/filebench/Makefile` |
| `bench.sh` calling wrong binary under `sudo` | Added `FILEBENCH=/home/evie/filebench/filebench` variable |
| `Out of shared memory` with 1M files | Raised `FILEBENCH_NFILESETENTRIES` to `2*1024*1024` in `~/filebench/ipc.h` |
| Run 2 re-creating 1M-file tree (slow + incomparable) | Added `reuse` flag to bigfileset definition |
| `lsof +D` hanging on 1M-file mount | Replaced with `fuser -m` |
| No cache flush between runs | Added `sync && echo 3 > /proc/sys/vm/drop_caches` in `bench.sh` |
| Remapper finding 0 pool files | Root cause under investigation; diagnostic output added to `load_pool` |

**Run 2 status**: remapper still fails to load pool files (0 found). Until this
is resolved, Run 2 is filebench-alone (no actual remapping), making the
comparison invalid. The baseline number above is valid.

## Known TODOs in the kernel code

| Location | TODO |
|---|---|
| `evfs.c` top | Increment `i_version` in evfs functions that modify inode metadata |
| `evfs_extent_move` | Copy data to the new physical block |
| `extent_move.sh` | Free the evicted physical block; add fsck check; fix pass/fail logic |

---

## Next steps

### Immediate: fix remapper pool loading

The remapper reports "need at least 2 files in pool, found 0" on every Run 2.
Diagnostic output was added to `load_pool` (prints `skipped_type`,
`skipped_stat`, `skipped_phys` counts + per-file FIEMAP error). Run `bench.sh`
and inspect stderr to identify the exact failure point.

### Macrobenchmark experiment plan

Four scenarios to characterise evfs-based defragmentation:

| # | Scenario | Purpose |
|---|---|---|
| 1 | **Young filesystem** — fresh ext4, filebench workload | Baseline (no fragmentation) |
| 2 | **Aged filesystem** — Geriatrix-aged ext4, filebench workload | Cost of fragmentation |
| 3 | **Aged + live defrag** — aged ext4, filebench + evfs defragmenter running simultaneously | Does live defrag recover throughput? |
| 4 | **Defrag throughput** — aged ext4, evfs defragmenter alone | How fast can evfs defrag? |

Scenarios 1 vs 2 quantify fragmentation overhead. Scenarios 2 vs 3 show
whether the evfs defragmenter recovers it. Scenario 4 measures defragmenter
speed in isolation.

---

### Step 1 (completed): filesystem aging with Geriatrix

#### Tool

**Geriatrix** ([github.com/saurabhkadekodi/geriatrix](https://github.com/saurabhkadekodi/geriatrix),
USENIX ATC '18). Drives a create/delete workload using empirically measured
file-age distributions to produce both file fragmentation and free-space
fragmentation. Seeded for reproducibility.

Built at `/opt/bin/geriatrix`:
```bash
sudo apt-get install cmake libboost-all-dev
git clone https://github.com/saurabhkadekodi/geriatrix ~/geriatrix
cd ~/geriatrix && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/opt .. && make && sudo make install
```

Profile files live in `~/geriatrix/profiles/{agrawal,dabre,douceur,grundman,meyer,pramod,wang_lanl,wang_os}/`.

#### Scripts

- **`test/filebench/setup_img.sh`** — unmounts, wipes, and reformats
  `evfs-sandbox-20gb.img` as a fresh 20 GB ext4 (1 KB blocks, 2 M inodes,
  `data=writeback`). Recreates `/mnt/fb` symlink.
- **`test/filebench/fragment.sh <img> [profile] [seed]`** — mounts the image,
  runs Geriatrix (default: `agrawal`, seed 42, 70% utilisation, 2-minute cap),
  reports `e2freefrag` histogram before and after.

#### Fragmentation results

Two runs performed. The second run used `-u 0.2` (20% fill) to leave headroom
for filebench's 1M-file dataset (~16 GB) and `-i 10` for deeper fragmentation.

**Key insight on file vs. free-space fragmentation**: Geriatrix's own files
always show avg extents/file ≈ 1.00 (contiguous) because they were written
sequentially. The fragmentation it produces is in the **free space** — scattered
holes from the create/delete cycles. New files written into this space (i.e.,
filebench's files) will be fragmented. `measure_fragmentation.sh` should be run
after filebench populates to observe the effect.

| Metric | Fresh image | Post-aging (80% util) | Post-aging (20% util, current) |
|---|---|---|---|
| Free space | 96.9% | 16.6% | ~80% |
| Free extents | 176 | 18,625 | in progress |
| Avg free extent | 115 MB | 187 KB | in progress |
| File fragmentation score | 1.00 | 1.00 | 1.00 (Geriatrix files) |

The 80% run left only 3.4 GB free — not enough for filebench. The 20% run
(currently executing) leaves ~16 GB free.

#### Fragmentation score metric

Measured with `measure_fragmentation.sh`:
- **Avg extents/file** — primary score; 1.0 = perfectly contiguous
- **% files with >1 extent** — secondary; counts files with any fragmentation
- **`e2freefrag` histogram** — free-space fragmentation (run on unmounted image)

#### Snapshot workflow

```bash
sudo ./test/filebench/setup_img.sh              # reformat fresh
sudo ./test/filebench/fragment.sh evfs-sandbox-20gb.img   # age it
cp evfs-sandbox-20gb.img evfs-aged-snap.img     # snapshot aged state
# restore before each benchmark run:
cp evfs-aged-snap.img evfs-sandbox-20gb.img
sudo mount -o loop,data=writeback evfs-sandbox-20gb.img evfs-sandbox-20gb
```

---

### Step 2 (immediate focus): defragmentation algorithm

Build `test/src/defrag.c` — a userspace defragmenter using evfs ioctls.

#### High-level structure

```
for each file in filesystem:
    score = fiemap_extent_count(file)
    if score <= FRAG_THRESHOLD:
        continue                      // skip unfragmented files
    defrag_file(mountfd, file)
```

**Fragmentation threshold**: skip files with extent count ≤ `FRAG_THRESHOLD`
(e.g., 1 — defrag any file with >1 extent; or a higher value to skip lightly
fragmented files and focus on worst cases).

#### Per-file defragmentation algorithm

```
defrag_file(mountfd, path):
    1. FIEMAP(path)  →  get current extents: [(log, phy, len), ...]
                         and iversion (exp_iver)
       if fm_mapped_extents == 1: return  // already contiguous, skip

    2. FSP_ITER(mountfd, start=0)  →  find free run of length >= file_blocks
       if no run large enough: skip file (or defrag partially)

    3. BLK_ALLOC(mountfd, block) for each block in free run
       retry on -EEXIST (TOCTOU race with normal allocator)

    4. open(path, O_RDONLY) + pread each extent → write to new physical blocks
       via direct block device access (/dev/loopX)
       (EXT_MV moves pointer only; data must be pre-populated)

    5. EXT_MV(mountfd, ino, exp_iver, log_start=0, new_phy, len=file_blocks)
       on -EAGAIN: re-read iversion + re-copy data + retry
       on success: file now points to contiguous new_phy run

    6. BLK_FREE(mountfd, old_phy) for each old physical block
```

#### Key design decisions

| Decision | Choice | Rationale |
|---|---|---|
| Granularity | Whole-file | Simpler; produces fully contiguous result; requires free run ≥ file size |
| Threshold | `extents > 1` | Defrag any fragmented file; can be tuned |
| Data copy path | `/dev/loopX` raw write to new physical blocks | Required since EXT_MV moves pointer only |
| EAGAIN retry | Re-read iver + re-copy + re-remap | iversion changes if file was written concurrently |
| EEXIST retry | Re-scan FSP_ITER from last position | Normal allocator claimed the block between scan and alloc |
| Skip condition | No free run ≥ file size | Log and move on; partially defragmenting a file is complex |

#### Implementation files

- `test/src/defrag.c` — main defragmenter binary
- `test/Makefile` — add `defrag` to `NAMES`
- `test/filebench/defrag.sh` — wrapper: mount image, run defrag, report fragmentation before/after

#### Open question: raw block write path

Step 4 requires writing data to physical block numbers on the loop device
before the inode points to them. Options:
- **`/dev/loopX` + `pwrite(fd, data, len, phy_block * FS_BLOCK_SIZE)`** — direct
  raw writes to the block device. Requires knowing which `/dev/loopX` device
  corresponds to the mounted image (`losetup -l` or `stat` the mount).
- **`fallocate` + `write` via VFS** — allocate a temp file in the free run,
  copy data, then `EXT_MV` both files. Avoids raw device access but is more
  complex.

The raw `/dev/loopX` approach is simpler to implement. This is the blocking
design question to resolve first.
