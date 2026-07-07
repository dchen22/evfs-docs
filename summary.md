# evfs extent_move — test summary

## What is evfs?

evfs (Extendable VFS) is a custom ioctl interface added to ext4 in this kernel
tree (`linux-6.8/fs/ext4/evfs.c`). It exposes filesystem internals — block
allocation, inode allocation, dentry manipulation, extent tree reads and
rewrites — directly to userspace. Operations that the normal VFS would only
allow indirectly (by going through write/truncate/etc.) can be issued as
explicit, targeted ioctls. All code lives in `linux-6.8/fs/ext4/`.

## What does `extent_move` do?

`EXT4_EVFS_EXT_MV` (`evfs_extent_move` in `evfs.c`) rewrites the physical
block pointer(s) in an inode's extent tree. The caller supplies:

```
(ino_num, exp_iver, log_start, phy_start, len)
```

The ioctl maps logical blocks `[log_start, log_start+len)` of the given inode
to physical blocks `[phy_start, phy_start+len)`. Crucially:

- It **does not copy data**. Physical block content is unchanged; only the
  inode's extent tree pointer is updated (there is a `// TODO: copy data`
  comment in the kernel source).
- The old physical block is evicted (its number ends up in `curr->phy_start`
  after `__evfs_extent_map`, but nothing is done with it — a `// TODO: free
  the block that are swapped out` exists in the test script).
- It is a **pointer reassignment**, not a reallocation.

### Kernel locking sequence

```c
inode_lock(inode);
down_write(&EXT4_I(inode)->i_data_sem);   // exclusive write lock

evfs_iver(inode, &iver);
if (iver != args->in.exp_iver) {
    ret = -EAGAIN;                         // iversion mismatch → abort before remap
    goto release_inode;
}

// ... extent tree rewrite under single journal transaction ...

up_write(&EXT4_I(inode)->i_data_sem);
inode_unlock(inode);
```

`down_write` **waits** for all current readers to finish and blocks new readers
for the full duration of the remap + journal commit. Nothing is ever aborted
mid-remap — once the lock is acquired and the iversion check passes, the ioctl
always runs to completion.

### Atomicity scope

One `EXT4_EVFS_EXT_MV` call is atomic over its entire `len`. Even if the
logical range spans multiple entries in the extent tree, the kernel's `repeat:`
loop handles each entry under the same `i_data_sem` write lock and single
journal transaction. A concurrent reader sees either the old mapping or the new
mapping, never a partial state.

The API accepts only **one** `(log_start, phy_start, len)` triple per call.
Remapping two disjoint logical ranges requires two separate ioctl calls; each
acquires and releases the lock independently, making the pair non-atomic.

## `iversion` and `EAGAIN`

`evfs_extent_move` checks the inode's `i_version` under the write lock and
returns `-EAGAIN` if it does not match `exp_iver`. The intended workflow is:

1. Caller reads current iversion via `EXT4_EVFS_IVER` (opens the file, reads
   `i_version`)
2. Caller calls `EXT4_EVFS_EXT_MV`, passing that iversion as `exp_iver`
3. Kernel acquires the write lock, re-reads iversion, and compares
4. If iversion changed between steps 1 and 3 → return `-EAGAIN`; caller
   re-reads state and retries
5. If iversion matches → proceed with the remap

`-EAGAIN` is standard Linux errno for "try again" — not a permanent error, just
a signal that the optimistic pre-flight check failed.

Two important caveats:

1. **Reads do not bump `i_version`**. Normal `read()` / `pread()` calls never
   modify `i_version`, so a concurrent reader cannot trigger EAGAIN.
2. **`EXT_MV` itself does not bump `i_version`** either. There is an explicit
   `// TODO: increment i_version in any evfs functions that modify inode
   metadata` at the top of `evfs.c`. Because neither the reader nor the writer
   increments `i_version`, EAGAIN never fires in practice — confirmed by the
   stress test returning `writer EAGAIN: 0`.

## Existing tests

| Script | What it tests |
|---|---|
| `test/extent_move.sh` | Basic single extent move: writes 4 KB of 0xAA, allocates a new block, remaps the first logical block of the file to it. Verifies the first block now reads as zeros (new empty block) and blocks 1–3 still read as 0xAA. |
| `test/extent_move_conc.sh` | Atomicity + contention stress test (described below). |

### `extent_move.sh` known bug

The pass/fail check at the end of `extent_move.sh` references `$res`, which is
only assigned in the `--native` branch. In the default (evfs) branch `$res` is
always empty, so the test always prints `Failed` even when the extent move
succeeded. The hexdump output visible in the terminal confirms the operation
worked correctly despite the false failure.

## Concurrency / atomicity stress test (`extent_move_conc`)

### Setup

- `file_a`: 16 KB (16 × 1 KB blocks), filled with `0xAA`. Physical blocks = PA.
- `file_b`: 16 KB (16 × 1 KB blocks), filled with `0xBB`. Physical blocks = PB.
- Physical block addresses obtained at startup via `FS_IOC_FIEMAP`.
- Both threads are pinned to separate CPUs via `pthread_setaffinity_np`
  (writer→CPU0, reader→CPU1) to guarantee true hardware parallelism rather
  than just OS-level concurrency.

**Writer thread**: alternates remapping `file_a`'s extent between PA and PB
using `EXT4_EVFS_EXT_MV`.

**Reader thread**: reads `file_a` continuously with `O_DIRECT` (bypasses page
cache; forces a fresh extent-tree lookup on every `pread`). Every byte in each
read must be the same value — a mix of `0xAA` and `0xBB` within a single read
means the remap was observed mid-flight.

Each thread reports the CPU it is actually running on at startup via
`sched_getcpu()` for independent validation that pinning took effect.

### Output fields

| Field | Meaning |
|---|---|
| `writer swaps` | Number of complete PA↔PB remaps performed |
| `writer ioctl calls` | Number of `EXT4_EVFS_EXT_MV` calls. Equals swaps in mode 0; equals 2× swaps in mode 1 |
| `writer EAGAIN` | Times the iversion check fired. Expected 0 (see iversion section above) |
| `writer saw reader` | Lower-bound count of times writer entered `ext_mv` while reader was in `pread` — writer's `down_write` may have had to wait for reader's read lock |
| `reader reads` | Number of complete 16 KB O_DIRECT reads. Independent of swap count; purely a function of I/O speed |
| `reader saw writer` | Lower-bound count of times reader entered `pread` while writer was in `ext_mv` — reader's `down_read` may have had to wait for the write lock |
| `corrupt reads` | Reads where bytes were not all the same value (mix of 0xAA and 0xBB). Any nonzero value = atomicity violation |

### Mode 0 — single ioctl (atomic)

One ioctl remaps all 16 blocks at once under a single write lock acquisition.

**Results (representative run, 10 s, CPU-pinned, 4 CPUs online):**

```
writer swaps:        449150
writer ioctl calls:  449150
writer EAGAIN:       0
writer saw reader:   102544
reader reads:        95384
reader saw writer:   35320
corrupt reads:       0
```

Zero corrupt reads across ~95k read attempts and ~449k writer swaps, with
~102k confirmed contention events. Single-ioctl atomicity is confirmed under
real, sustained, parallel contention.

### Mode 1 — two ioctls (non-atomic)

Two separate ioctl calls each remap 8 blocks (first half, then second half).
Between the two calls the inode's extent tree is half-remapped, and a
concurrent reader can observe this state.

**Results (representative run, 10 s, CPU-pinned):**

```
writer swaps:        134115
writer ioctl calls:  268230
writer EAGAIN:       0
writer saw reader:   98871
reader reads:        106904
reader saw writer:   63885
corrupt reads:       57336
```

57,336 of 106,904 reads (~53.6%) were corrupt. Every corrupt read showed
exactly `0xAA=8192 0xBB=8192` — the split always precisely at the 8-block
(8 KB) boundary, confirming the tear is deterministic and located exactly at
the ioctl boundary. The ~50% rate is expected: the writer spends roughly equal
time in the half-remapped state as in the fully-remapped state.

`writer ioctl calls = 2 × writer swaps` confirms each swap issued exactly two
ioctl calls as intended.

### Mode comparison

| Metric | Mode 0 (single ioctl) | Mode 1 (two ioctls) |
|---|---|---|
| Writer swaps/s | ~45k | ~13k |
| Writer ioctl calls/swap | 1 | 2 |
| Corrupt reads | 0 | ~53% |
| `reader saw writer` | ~35k | ~64k |

The writer is ~3.4× faster in mode 0 because each swap is one syscall instead
of two plus an extra `get_iver()` call in between. `reader saw writer` is
higher in mode 1 because there are 2× as many ioctl calls, giving the reader
more windows to arrive while the writer is mid-ioctl — even though each
individual lock hold is shorter.

## Contention measurement

### How it works

Each thread sets an atomic flag while it is in its "active region" (between
flag set and flag clear). The other thread checks that flag before entering its
own active region:

```
writer:
    check reader_in_pread → if set: writer_saw_reader++
    set writer_in_ioctl = 1
    ioctl(EXT4_EVFS_EXT_MV, ...)
    set writer_in_ioctl = 0

reader:
    check writer_in_ioctl → if set: reader_saw_writer++
    set reader_in_pread = 1
    pread(...)
    set reader_in_pread = 0
```

### Is the current method a true lower bound?

**Yes.** A lower bound means reported count ≤ actual kernel contentions. If the
count is nonzero, actual contention definitely occurred.

The "check before set" pattern satisfies this. When a counter increments, the
checking thread observed the other's flag set while about to enter its own
syscall — the other thread was genuinely in its active region, and the checking
thread entered its syscall immediately after. The check cannot fire without real
concurrent activity.

The method can **undercount** — there is a race where both threads check before
either has set its flag:

```
t1  writer: check reader_in_pread → 0   (reader not yet active)
t2  reader: check writer_in_ioctl → 0   (writer not yet active)
t3  writer: set writer_in_ioctl = 1
t4  reader: set reader_in_pread = 1
t5  writer: enter ext_mv → down_write   (may have to wait for reader)
t6  reader: enter pread  → down_read    (blocks on writer's write lock)
    ← actual kernel lock contention, but neither counter incremented
```

The guarantee is one-directional: **a nonzero count proves contention occurred;
a zero count does not prove it didn't.**

### Why "set first, check" would NOT be a lower bound

Flipping the order — setting your own flag before checking the other's — would
**overcount**: the flag is set in userspace slightly before the syscall is
entered. The other thread could see the flag and increment its counter, but
then the first thread's syscall could complete so quickly that the lock was
never actually contested by the time the other thread reaches `down_write` /
`down_read`. That is a false positive — a counted event with no actual kernel
wait — which violates the lower bound guarantee.

| Approach | Guarantee |
|---|---|
| Current (check before set) | Lower bound — nonzero count proves contention occurred |
| Set first, check | Upper bound approximation — can overcount |

### Perfect measurement (exact count)

The current method is a lower bound but can miss events. An exact count
requires kernel instrumentation:

- **eBPF**: attach to `rwsem_down_write_slowpath` and
  `rwsem_down_read_slowpath`; filter by the specific inode's `i_data_sem`
  address. These functions are called only when a lock acquisition actually
  blocks.
- **`CONFIG_LOCK_STAT`**: enables per-lock contention counters in the kernel;
  query via `/proc/lock_stat`.
- **ftrace / tracepoints**: trace `rwsem_*` events.
- **Kernel-side counter in `evfs_extent_move`**: add a counter incremented only
  when `down_write` had to block (e.g. by checking `rwsem_is_contended` before
  the call, or timing `down_write` inside the kernel).

## CPU pinning

Threads are pinned to separate CPUs using `pthread_setaffinity_np` to guarantee
true hardware parallelism. Each thread reports its actual CPU at startup via
`sched_getcpu()` for independent validation.

**Known limitation**: `pthread_setaffinity_np` is called from the main thread
after `pthread_create`. There is a small window where a thread may start
executing before the pin takes effect, causing `sched_getcpu()` to report the
wrong CPU. This was observed in one run where test 2 reported `writer running
on CPU 1 / reader running on CPU 2` instead of CPU 0 and CPU 1. The affinity
is still applied correctly for the duration of the test; only the initial
self-report may be inaccurate.

The fix, if exact reporting is required, is to have each thread set its own
affinity using `sched_setaffinity(0, ...)` as the very first line of the thread
function, with the desired CPU passed via the context struct.

## Known TODOs in the kernel code

| Location | TODO |
|---|---|
| `evfs.c` top | Increment `i_version` in all evfs functions that modify inode metadata. Until this is done, EAGAIN never fires in `evfs_extent_move`. |
| `evfs_extent_move` | Copy data to the new physical block (currently only the pointer is updated; the new block retains whatever bytes were already there). |
| `extent_move.sh` | Free the physical block that was evicted after a remap. |
| `extent_move.sh` | Add fsck and debugfs check after unmount to verify on-disk consistency. |
| `extent_move.sh` | Fix the pass/fail check: capture hexdump into `$res` in the non-native branch. |
