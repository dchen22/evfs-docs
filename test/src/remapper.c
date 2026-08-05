/*
 * remapper.c – randomised multi-file remapper for macrobenchmark use.
 *
 * Loads all files from a pool directory, then continuously picks random
 * pairs of same-size files and swaps their extents via EXT4_EVFS_EXT_MV.
 * Intended to run alongside filebench to create realistic journal and
 * inode-level contention across many files.
 *
 * Usage: remapper <mountpt> <pool_dir> <secs>
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/evfs.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define FS_BLOCK_SIZE  1024
#define MAX_EXTENTS    64
#define MAX_FILES      4096

static volatile int g_stop = 0;
static void on_alarm(int sig) { (void)sig; g_stop = 1; }

typedef struct {
	char               path[512];
	unsigned long      ino;
	unsigned long long phy_start;
	unsigned int       num_blocks;
} rfile_t;

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

static int refresh_phys(rfile_t *f)
{
	struct {
		struct fiemap        fm;
		struct fiemap_extent ext[MAX_EXTENTS];
	} buf;

	int fd = open(f->path, O_RDONLY);
	if (fd < 0) return -1;

	memset(&buf, 0, sizeof(buf));
	buf.fm.fm_start        = 0;
	buf.fm.fm_length       = FIEMAP_MAX_OFFSET;
	buf.fm.fm_extent_count = MAX_EXTENTS;

	if (ioctl(fd, FS_IOC_FIEMAP, &buf.fm) < 0) {
		close(fd); return -1;
	}
	close(fd);

	if (buf.fm.fm_mapped_extents == 0) return -1;

	f->phy_start  = buf.fm.fm_extents[0].fe_physical / FS_BLOCK_SIZE;
	unsigned int total = 0;
	for (uint32_t i = 0; i < buf.fm.fm_mapped_extents; i++)
		total += (unsigned int)(buf.fm.fm_extents[i].fe_length / FS_BLOCK_SIZE);
	f->num_blocks = total;
	return 0;
}

static int load_pool(const char *dir, rfile_t *pool, int max)
{
	DIR *d = opendir(dir);
	if (!d) { perror("opendir"); return -1; }

	int n = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) && n < max) {
		if (ent->d_type != DT_REG) continue;

		snprintf(pool[n].path, sizeof(pool[n].path),
			 "%s/%s", dir, ent->d_name);

		struct stat st;
		if (stat(pool[n].path, &st) < 0) continue;
		pool[n].ino = (unsigned long)st.st_ino;

		if (refresh_phys(&pool[n]) < 0) continue;
		n++;
	}
	closedir(d);
	return n;
}

int main(int argc, char *argv[])
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s <mountpt> <pool_dir> <secs>\n",
			argv[0]);
		return 1;
	}

	const char *mountpt  = argv[1];
	const char *pool_dir = argv[2];
	int         secs     = atoi(argv[3]);

	rfile_t *pool = calloc(MAX_FILES, sizeof(rfile_t));
	if (!pool) { perror("calloc"); return 1; }

	int n = load_pool(pool_dir, pool, MAX_FILES);
	if (n < 2) {
		fprintf(stderr, "need at least 2 files in pool, found %d\n", n);
		free(pool); return 1;
	}
	printf("remapper: loaded %d files from %s, duration=%ds\n",
	       n, pool_dir, secs);

	int mnt_fd = open(mountpt, O_RDONLY);
	if (mnt_fd < 0) { perror("open mountpt"); free(pool); return 1; }

	srand((unsigned)getpid());
	signal(SIGALRM, on_alarm);
	alarm((unsigned)secs);

	long swaps = 0, calls = 0, eagain = 0, skipped = 0;

	while (!g_stop) {
		/* Pick two distinct random files with the same block count */
		int i = rand() % n;
		int j = rand() % n;
		if (i == j) continue;
		if (pool[i].num_blocks != pool[j].num_blocks) {
			skipped++;
			continue;
		}

		unsigned int len = pool[i].num_blocks;

		/* Swap i -> j */
		unsigned long long iver = get_iver(pool[i].path);
		struct ext4_evfs_ext_mv_args a;
		memset(&a, 0, sizeof(a));
		a.in.ino_num       = pool[i].ino;
		a.in.exp_iver      = iver;
		a.in.ext.log_start = 0;
		a.in.ext.phy_start = pool[j].phy_start;
		a.in.ext.len       = len;

		int ret = ioctl(mnt_fd, EXT4_EVFS_EXT_MV, &a);
		calls++;
		if (ret < 0) {
			if (errno == EAGAIN) { eagain++; continue; }
			perror("ext_mv"); break;
		}

		/* Swap j -> old phy of i */
		unsigned long long old_phy_i = pool[i].phy_start;
		iver = get_iver(pool[j].path);
		memset(&a, 0, sizeof(a));
		a.in.ino_num       = pool[j].ino;
		a.in.exp_iver      = iver;
		a.in.ext.log_start = 0;
		a.in.ext.phy_start = old_phy_i;
		a.in.ext.len       = len;

		ret = ioctl(mnt_fd, EXT4_EVFS_EXT_MV, &a);
		calls++;
		if (ret < 0) {
			if (errno == EAGAIN) { eagain++; }
			else { perror("ext_mv"); break; }
		}

		/* Update cached phy_start for both files */
		unsigned long long tmp = pool[i].phy_start;
		pool[i].phy_start = pool[j].phy_start;
		pool[j].phy_start = tmp;

		swaps++;
	}

	close(mnt_fd);
	free(pool);

	printf("remapper done: swaps=%ld ioctl_calls=%ld eagain=%ld "
	       "skipped(size_mismatch)=%ld eagain_rate=%.2f%%\n",
	       swaps, calls, eagain, skipped,
	       calls ? 100.0 * eagain / calls : 0.0);

	return 0;
}
