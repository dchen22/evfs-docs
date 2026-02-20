#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include "evfs_cmd.h"

int main(int argc, char * argv[]) {
	if (argc != 2) {
		fprintf(stderr, "usage: program_name mountpoint");
		return 1;
	}

	char mountpt[64];
	strncpy(mountpt, argv[1], 64);

	FILE *test;
	test = fopen(mountpt, "r");
	int fd = fileno(test);
	ioctl(fd, EXT4_IOC_EVFS_HELLO);

	return 0;
}
