#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ext4_evfs_inode_remap {
    uint64_t inode_number; // input
    uint64_t start_block;  // input
    uint32_t length;       // input
    uint32_t _padding;     // alignment
};

#define EXT4_IOC_INODE_REMAP _IOW('f', 108, struct ext4_evfs_inode_remap)

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: ./inode_remap [inode num] [extent block] [extent length]");
        return 1;
    }
    int fd;
    fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    int inode_number = strtoul(argv[1], NULL, 10);
    int start_block = strtoul(argv[2], NULL, 10);
    int extent_length = strtoul(argv[3], NULL, 10);

    struct ext4_evfs_inode_remap remap_info = {
        .inode_number = inode_number,
        .start_block = start_block,
        .length = extent_length
    };

    if (ioctl(fd, EXT4_IOC_INODE_REMAP, &remap_info) < 0) {
        perror("ioctl");
    }

    printf("Finished inode remapping\n");

    return 0;
}