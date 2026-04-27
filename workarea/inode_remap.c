#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ext4_evfs_extent {
    uint64_t start_block;   // physical block number where this extent starts
    uint32_t length;        // length in blocks
};

struct ext4_evfs_inode_remap {
    uint64_t inode_number;                         // input
    struct ext4_evfs_extent *extents;    // input: array of new extents
    uint32_t num_extents;                          // input: len(extents)
};

#define EXT4_IOC_INODE_REMAP _IOW('f', 108, struct ext4_evfs_inode_remap)

int main(int argc, char *argv[]) {
    if (argc < 4 || argc % 2 != 0) {
        printf("Usage: ./inode_remap [inode num] [extent1 block] [extent1 length] ...");
        return 1;
    }

    int fd;
    struct ext4_evfs_extent *extents;
    uint32_t num_extents = 0;
    struct ext4_evfs_inode_remap remap_info = {0};

    fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    num_extents = (argc - 2) / 2;
    extents = malloc(sizeof(struct ext4_evfs_extent) * num_extents);
    if (extents == NULL) {
        printf("malloc failed\n");
        return 1;
    }
    remap_info.inode_number = strtoul(argv[1], NULL, 10);
    remap_info.num_extents = num_extents;
    remap_info.extents = extents;

    int start_block, extent_length;
    for (int i = 0; i < num_extents; i++) {
        start_block = strtoul(argv[2 + i * 2], NULL, 10);
        extent_length = strtoul(argv[3 + i * 2], NULL, 10);

        extents[i].start_block = start_block;
        extents[i].length = extent_length;
    }

    if (ioctl(fd, EXT4_IOC_INODE_REMAP, &remap_info) < 0) {
        perror("ioctl");
        return 1;
    }

    printf("Finished inode remapping\n");
    free(extents);

    return 0;
}