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

#### Pilot run results (agrawal profile, 2 min, seed=42)

Geriatrix ran for exactly 2 minutes on the 20 GB image, performing 210,000
operations (3 disk overwrites = ~60 GB of create/delete workload), reaching
90% chi-squared convergence.

| Metric | Pre-aging (fresh) | Post-aging |
|---|---|---|
| Free space | 96.9% (20.3 M blocks) | 26.9% (5.6 M blocks) |
| Free extents | 176 | **18,297** |
| Avg free extent size | 115 MB | **308 KB** |
| Max free extent size | 128 MB | 16 MB |
| Min free extent size | 4.5 MB | **1 KB** |

Free-space fragmentation is severe: 104× more free extents, each 374× smaller
on average. This is the input state for the aged-filesystem benchmark scenarios.

**Free-space histogram (post-aging):**
```
 1K...  2K:  5035 extents   (0.09% of free blocks)
 2K...  4K:  3042 extents   (0.11%)
 8K... 16K:  2366 extents   (0.34%)
32K... 64K:  1892 extents   (1.07%)
 1M...  2M:   590 extents  (13.90%)
 2M...  4M:   684 extents  (28.93%)
 4M...  8M:   353 extents  (32.24%)
```

#### Snapshot workflow

```bash
sudo ./test/filebench/setup_img.sh
sudo ./test/filebench/fragment.sh evfs-sandbox-20gb.img
cp evfs-sandbox-20gb.img evfs-aged-snap.img    # save aged state
# restore before each benchmark run:
cp evfs-aged-snap.img evfs-sandbox-20gb.img
sudo mount -o loop,data=writeback evfs-sandbox-20gb.img evfs-sandbox-20gb
```

---

### Step 2: defragmentation algorithm

Once aging is working and we have a measurable fragmented state, build a
defragmenter on top of `EXT4_EVFS_EXT_MV`:

1. **Find fragmented files** — `FS_IOC_FIEMAP`; skip files with 1 contiguous extent
2. **Find free run** — `EXT4_EVFS_FSP_ITER`; need run ≥ file size (or extent size
   for per-extent mode)
3. **Claim blocks** — `EXT4_EVFS_BLK_ALLOC` per block; retry on `-EEXIST`
4. **Copy data** — read from old physical blocks, write to new ones (required
   because `EXT_MV` moves the pointer, not the data)
5. **Remap** — `EXT4_EVFS_EXT_MV` with `exp_iver` from step 1; retry on `-EAGAIN`
6. **Free old blocks** — `EXT4_EVFS_BLK_FREE`

Key design decisions to resolve: whole-file vs. per-extent granularity; retry
budget on `-EAGAIN`; live vs. offline mode (live needs the retry loop;
offline on a read-only mount does not).
