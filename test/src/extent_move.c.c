#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/evfs.h>
#include "helper.h"

int main(int argc, char * argv[]) {
	if (usage(argc, 7, "extent_move.custom iver ino_num log_start phy_start len img"))
		return 1;

	struct ext4_evfs_ext_mv_args args;
	if (str2ull(argv[1], &(args.in.exp_iver), "expected i_version")) return 1;
	if (str2ul(argv[2], &(args.in.ino_num), "inode number")) return 1;
	if (str2u(argv[3], &(args.in.ext.log_start), "logical start")) return 1;
	if (str2ull(argv[4], &(args.in.ext.phy_start), "phyiscal start")) return 1;
	if (str2u(argv[5], &(args.in.ext.len), "length")) return 1;	

	struct ext4_evfs_ext exts[args.in.ext.len];
	args.out.exts = exts;

	char fimg[_TEVFS_EXT4_PATHLEN];
	null_terminated_strncpy(fimg, argv[6], _TEVFS_EXT4_PATHLEN);

	int fd = open(fimg, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	// call ioctl
	if (ioctl(fd, EXT4_EVFS_EXT_MV, &args) < 0) {
		perror("ioctl >> extent move");
		close(fd);
		return 1;
	}

	printf("=== extents detached ===\n");
	printf("log\tphy\tlen\n");
	for (unsigned int i = 0; i < args.out.num_exts; i++) {
		printf("%u\t%llu\t%u\n",
			args.out.exts[i].log_start,
			args.out.exts[i].phy_start,
			args.out.exts[i].len);
	}

	close(fd);
	return 0;
}
