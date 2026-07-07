/*
 * extent_move_conc.c – atomicity + contention stress test for EXT4_EVFS_EXT_MV.
 *
 * Setup:
 *   file_a  (N blocks, all 0xAA bytes)  ← subject under test
 *   file_b  (N blocks, all 0xBB bytes)  ← donor (physical blocks as swap target)
 *
 * Writer thread continuously remaps file_a's extent between PA (0xAA) and
 * PB (0xBB).  Reader thread writes a rolling byte value (0x00..0xFF cycling)
 * then immediately reads back; if the remapper swapped the extent between the
 * pwrite and pread the read returns stale data from the other physical block.
 *
 * Atomicity check: every byte in the pread result must be the same value as
 * every other byte.  A uniform read (all 0xAA, all 0xBB, all write_color,
 * etc.) is fine — it just means an atomic swap happened between write and
 * read.  A mixed read (some bytes 0xAA, others 0xBB, or first half one value
 * and second half another) means a partial/torn remap was visible mid-flight.
 *
 * Contention counters (userspace proxies for kernel lock contention):
 *
 *   writer_saw_reader - each time the writer enters ext_mv while the
 *     reader is inside pread.  The writer's down_write(i_data_sem) will
 *     have to wait for that in-flight read lock to drain before it can
 *     take the exclusive lock.
 *
 *   reader_saw_writer - each time the reader enters pread while the
 *     writer is inside ext_mv.  The reader's down_read(i_data_sem) will
 *     block until the writer releases the write lock.
 *
 * Both counters are approximate (the flags are set in userspace before
 * entering the kernel, so there is a small window around the syscall
 * boundary), but they reliably track the overlap frequency and let us
 * compare mode 0 vs mode 1: mode 0 holds the write lock for all N blocks
 * at once, so reader_saw_writer should be higher; mode 1 holds it for
 * only N/2 blocks at a time, giving the reader more free windows.
 *
 * EAGAIN count: evfs_extent_move checks i_version under the write lock
 * and returns -EAGAIN if it changed since the caller last read it.
 * pwrite() bumps i_version, so EAGAIN fires whenever the reader thread
 * writes to the file between the writer's get_iver() and the ioctl.
 *
 * Mode 0 – single ioctl  (atomic, no corruption expected)
 * Mode 1 – two ioctls    (non-atomic, corruption expected)
 *
 * Usage: extent_move_conc <mountpt> <file_a> <file_b> <mode> <secs>
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/evfs.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define FS_BLOCK_SIZE  1024
#define COLOR_A        ((uint8_t)0xAA)
#define COLOR_B        ((uint8_t)0xBB)
#define MAX_EXTENTS    64
#define READ_DELAY_US  100   /* µs between pwrite and pread; widens the
                                window for the remapper to land in */

/* ------------------------------------------------------------------ */

typedef struct {
	char mountpt[256];
	char path_a[256];
	char path_b[256];
	unsigned long      ino_a;
	unsigned long      ino_b;
	unsigned long long phy_a;       /* physical block start of file_a */
	unsigned long long phy_b;       /* physical block start of file_b */
	unsigned int       num_blocks;
	int                multi_ioctl; /* 0 = single call, 1 = two calls */
	int                duration_secs;

	atomic_int stop;

	/* set while each thread is inside its syscall - read by the other */
	atomic_int reader_in_pread;
	atomic_int writer_in_ioctl;

	/* writer metrics */
	long writer_swaps;
	long writer_ioctl_calls;
	long writer_eagain;
	long writer_saw_reader; /* entered ext_mv while reader was in pread */

	/* reader metrics */
	long reader_reads;
	long reader_corrupt;
	long reader_saw_writer; /* entered pread while writer was in ext_mv */
} ctx_t;

/* ------------------------------------------------------------------ */

static int pin_thread(pthread_t tid, int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return pthread_setaffinity_np(tid, sizeof(set), &set);
}

static unsigned long long get_iver(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror("open (iver)"); return 0; }
	unsigned long long iver = 0;
	if (ioctl(fd, EXT4_EVFS_IVER, &iver) < 0)
		perror("ioctl EXT4_EVFS_IVER");
	close(fd);
	return iver;
}

/*
 * Issue one ext_mv call, updating contention + EAGAIN counters.
 * The writer_in_ioctl flag is set for the duration so the reader can
 * detect it.
 */
static void tracked_ext_mv(ctx_t *ctx, int mnt_fd,
			    unsigned long long iver,
			    unsigned int log_start,
			    unsigned long long phy_start,
			    unsigned int len)
{
	/* Check for contention before entering the syscall */
	if (atomic_load(&ctx->reader_in_pread))
		ctx->writer_saw_reader++;

	struct ext4_evfs_ext_mv_args a;
	memset(&a, 0, sizeof(a));
	a.in.ino_num       = ctx->ino_a;
	a.in.exp_iver      = iver;
	a.in.ext.log_start = log_start;
	a.in.ext.phy_start = phy_start;
	a.in.ext.len       = len;

	atomic_store(&ctx->writer_in_ioctl, 1);
	int ret = ioctl(mnt_fd, EXT4_EVFS_EXT_MV, &a);
	atomic_store(&ctx->writer_in_ioctl, 0);

	ctx->writer_ioctl_calls++;
	if (ret < 0) {
		if (errno == EAGAIN)
			ctx->writer_eagain++;
		else
			perror("ext_mv");
	}
}

/*
 * Use FIEMAP to get the physical block start and total block count.
 */
static int get_phys_info(const char *path,
			 unsigned long long *phy_start, unsigned int *num_blocks)
{
	struct {
		struct fiemap        fm;
		struct fiemap_extent ext[MAX_EXTENTS];
	} buf;

	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror("open (fiemap)"); return -1; }

	memset(&buf, 0, sizeof(buf));
	buf.fm.fm_start        = 0;
	buf.fm.fm_length       = FIEMAP_MAX_OFFSET;
	buf.fm.fm_extent_count = MAX_EXTENTS;

	if (ioctl(fd, FS_IOC_FIEMAP, &buf.fm) < 0) {
		perror("ioctl FS_IOC_FIEMAP");
		close(fd);
		return -1;
	}
	close(fd);

	if (buf.fm.fm_mapped_extents == 0) {
		fprintf(stderr, "fiemap: no extents for %s\n", path);
		return -1;
	}
	if (buf.fm.fm_mapped_extents > 1)
		fprintf(stderr, "warning: %s has %u extents; test works best "
			"with a single contiguous extent\n",
			path, buf.fm.fm_mapped_extents);

	*phy_start = buf.fm.fm_extents[0].fe_physical / FS_BLOCK_SIZE;
	unsigned int total = 0;
	for (uint32_t i = 0; i < buf.fm.fm_mapped_extents; i++)
		total += (unsigned int)(buf.fm.fm_extents[i].fe_length / FS_BLOCK_SIZE);
	*num_blocks = total;
	return 0;
}

/* ------------------------------------------------------------------ */

static void *writer_fn(void *arg)
{
	ctx_t *ctx = (ctx_t *)arg;

	printf("writer running on CPU %d\n", sched_getcpu());

	int mnt_fd = open(ctx->mountpt, O_RDONLY);
	if (mnt_fd < 0) { perror("open mountpt (writer)"); return NULL; }

	int state = 0; /* 0: file_a -> PA, 1: file_a->PB */
	long swaps = 0;
	unsigned int n    = ctx->num_blocks;
	unsigned int half = n / 2;

	while (!atomic_load(&ctx->stop)) {
		unsigned long long iver   = get_iver(ctx->path_a);
		unsigned long long newphy = (state == 0) ? ctx->phy_b : ctx->phy_a;

		if (!ctx->multi_ioctl) {
			tracked_ext_mv(ctx, mnt_fd, iver, 0, newphy, n);
		} else {
			tracked_ext_mv(ctx, mnt_fd, iver, 0, newphy, half);
			iver = get_iver(ctx->path_a);
			tracked_ext_mv(ctx, mnt_fd, iver, half, newphy + half, n - half);
		}

		state ^= 1;
		swaps++;
	}

	ctx->writer_swaps = swaps;
	close(mnt_fd);
	return NULL;
}

static void *reader_fn(void *arg)
{
	ctx_t *ctx = (ctx_t *)arg;

	printf("reader running on CPU %d\n", sched_getcpu());

	size_t file_size = (size_t)ctx->num_blocks * FS_BLOCK_SIZE;

	void *buf;
	if (posix_memalign(&buf, 4096, file_size) != 0) {
		perror("posix_memalign");
		return NULL;
	}

	int direct = 1;
	int fd = open(ctx->path_a, O_RDWR | O_DIRECT);
	if (fd < 0) {
		fprintf(stderr, "O_DIRECT unavailable (%s); using DONTNEED fallback\n",
			strerror(errno));
		direct = 0;
		fd = open(ctx->path_a, O_RDWR);
		if (fd < 0) { perror("open file_a (reader)"); free(buf); return NULL; }
	}

	long reads = 0, corrupt = 0, saw_writer = 0;

	/*
	 * write_color cycles 0x00..0xFF each iteration.  Using a fresh value
	 * per write prevents the physical blocks from converging to the same
	 * content (which would mask corruption detection).
	 */
	uint8_t write_color = 0x00;

	while (!atomic_load(&ctx->stop)) {
		/* Refill buffer with this iteration's write value */
		memset(buf, write_color, file_size);

		if (!direct)
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

		/* Check for contention before entering the syscalls */
		if (atomic_load(&ctx->writer_in_ioctl))
			saw_writer++;

		/*
		 * Write write_color then immediately read back.  If the
		 * remapper swaps the extent between the write and the read,
		 * the read returns stale data from the other physical block
		 * rather than the value we just wrote.
		 */
		atomic_store(&ctx->reader_in_pread, 1);
		pwrite(fd, buf, file_size, 0);
		if (READ_DELAY_US > 0)
			usleep(READ_DELAY_US);
		ssize_t n = pread(fd, buf, file_size, 0);
		atomic_store(&ctx->reader_in_pread, 0);

		if (n <= 0) { write_color++; continue; }

		uint8_t *b = (uint8_t *)buf;

		/*
		 * Torn-read check: all bytes must be the same value.
		 * A uniform read (wrong value but consistent) is fine — it
		 * just means an atomic swap completed between pwrite and pread.
		 * A mixed read means a partial remap was visible mid-flight.
		 */
		uint8_t first = b[0];
		int bad = 0;
		for (ssize_t i = 1; i < n; i++) {
			if (b[i] != first) { bad = 1; break; }
		}

		reads++;
		if (bad) {
			corrupt++;
			if (corrupt <= 5) {
				int match = 0, aa = 0, bb = 0, other = 0;
				for (ssize_t i = 0; i < n; i++) {
					if      (b[i] == write_color) match++;
					else if (b[i] == COLOR_A)     aa++;
					else if (b[i] == COLOR_B)     bb++;
					else                          other++;
				}
				fprintf(stderr,
					"[reader] torn read #%ld: "
					"write_color=0x%02x "
					"write_color_count=%d 0xAA=%d 0xBB=%d other=%d\n",
					corrupt, write_color,
					match, aa, bb, other);
			}
		}

		write_color++;   /* uint8_t wraps 0xFF -> 0x00 automatically */
	}

	ctx->reader_reads      = reads;
	ctx->reader_corrupt    = corrupt;
	ctx->reader_saw_writer = saw_writer;
	free(buf);
	close(fd);
	return NULL;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	if (argc != 6) {
		fprintf(stderr,
			"usage: %s <mountpt> <file_a> <file_b> <mode> <secs>\n"
			"  mode 0 = single ioctl  (atomic;     no corruption expected)\n"
			"  mode 1 = two ioctls    (non-atomic; corruption expected)\n",
			argv[0]);
		return 1;
	}

	ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	strncpy(ctx.mountpt, argv[1], sizeof(ctx.mountpt) - 1);
	strncpy(ctx.path_a,  argv[2], sizeof(ctx.path_a)  - 1);
	strncpy(ctx.path_b,  argv[3], sizeof(ctx.path_b)  - 1);
	ctx.multi_ioctl   = atoi(argv[4]);
	ctx.duration_secs = atoi(argv[5]);

	struct stat sa, sb;
	if (stat(ctx.path_a, &sa) < 0) { perror("stat file_a"); return 1; }
	if (stat(ctx.path_b, &sb) < 0) { perror("stat file_b"); return 1; }
	ctx.ino_a = (unsigned long)sa.st_ino;
	ctx.ino_b = (unsigned long)sb.st_ino;

	unsigned int nb_b;
	if (get_phys_info(ctx.path_a, &ctx.phy_a, &ctx.num_blocks) < 0) return 1;
	if (get_phys_info(ctx.path_b, &ctx.phy_b, &nb_b)           < 0) return 1;

	if (ctx.num_blocks != nb_b) {
		fprintf(stderr, "block count mismatch: file_a=%u file_b=%u\n",
			ctx.num_blocks, nb_b);
		return 1;
	}
	if (ctx.num_blocks < 2) {
		fprintf(stderr, "need at least 2 blocks per file\n");
		return 1;
	}

	printf("=== extent_move atomicity stress test ===\n");
	printf("file_a:     %s  (ino=%lu  phy_block=%llu)\n",
	       ctx.path_a, ctx.ino_a, ctx.phy_a);
	printf("file_b:     %s  (ino=%lu  phy_block=%llu)\n",
	       ctx.path_b, ctx.ino_b, ctx.phy_b);
	printf("num_blocks: %u  (%zu bytes)\n",
	       ctx.num_blocks, (size_t)ctx.num_blocks * FS_BLOCK_SIZE);
	printf("mode:       %s\n",
	       ctx.multi_ioctl
	       ? "1 - two ioctls (non-atomic; corruption expected)"
	       : "0 - single ioctl (atomic; no corruption expected)");
	printf("duration:   %d s\n\n", ctx.duration_secs);

	atomic_init(&ctx.stop,           0);
	atomic_init(&ctx.reader_in_pread, 0);
	atomic_init(&ctx.writer_in_ioctl, 0);

	/* Pin writer and reader to separate CPUs for true parallelism.
	 * Fall back gracefully if the system has fewer than 2 CPUs. */
	int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
	int pin = (ncpus >= 2);
	if (pin)
		printf("pinning writer->CPU0, reader->CPU1 (%d CPUs online)\n",
		       ncpus);
	else
		printf("warning: only %d CPU online - affinity pinning skipped\n",
		       ncpus);

	pthread_t wt, rt;
	pthread_create(&wt, NULL, writer_fn, &ctx);
	pthread_create(&rt, NULL, reader_fn, &ctx);

	if (pin) {
		if (pin_thread(wt, 0))
			perror("pin writer");
		if (pin_thread(rt, 1))
			perror("pin reader");
	}

	sleep(ctx.duration_secs);
	atomic_store(&ctx.stop, 1);

	pthread_join(wt, NULL);
	pthread_join(rt, NULL);

	printf("\n=== results ===\n");
	printf("writer swaps:        %ld\n", ctx.writer_swaps);
	printf("writer ioctl calls:  %ld\n", ctx.writer_ioctl_calls);
	printf("writer EAGAIN:       %ld\n", ctx.writer_eagain);
	printf("writer saw reader:   %ld  (ext_mv entered while reader was in pread)\n",
	       ctx.writer_saw_reader);
	printf("reader reads:        %ld\n", ctx.reader_reads);
	printf("reader saw writer:   %ld  (pread entered while writer was in ext_mv)\n",
	       ctx.reader_saw_writer);
	printf("torn reads:          %ld\n", ctx.reader_corrupt);

	int pass;
	if (!ctx.multi_ioctl) {
		pass = (ctx.reader_corrupt == 0);
		printf("\n%s  (single-ioctl: %s)\n",
		       pass ? "PASS" : "FAIL",
		       pass ? "no torn reads - extent swap is atomic"
		            : "torn read observed - single-ioctl is NOT atomic!");
	} else {
		pass = (ctx.reader_corrupt > 0);
		printf("\n%s  (multi-ioctl: %s)\n",
		       pass ? "PASS" : "NOTE",
		       pass ? "torn reads observed - non-atomicity confirmed"
		            : "no torn reads caught - try a longer duration");
	}

	return pass ? 0 : 1;
}
