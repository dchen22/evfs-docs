#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NUM_EXTENTS 128

struct ext4_evfs_extent {
    uint64_t start_block;   // physical block number where this extent starts
    uint32_t length;        // length in blocks
};

struct ext4_evfs_get_inode_extents {
    uint64_t inode_number;         // input
    uint32_t max_num_extents;      // input: capacity of user buffer
    uint32_t result_num_extents;   // output: number of extents written to user buffer
    struct ext4_evfs_extent *extents;    // input: user buffer
};

#define EXT4_IOC_GET_INODE_EXTENTS _IOR('f', 107, struct ext4_evfs_get_inode_extents)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ./get_inode_extents [inode num]");
        return 1;
    }
    int fd;
    fd = open("/home/evie/code/evfs-sandbox/fileA", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    int inode_number = strtoul(argv[1], NULL, 10);
    struct ext4_evfs_extent extents[MAX_NUM_EXTENTS];
    memset(extents, 0, sizeof(extents));

    struct ext4_evfs_get_inode_extents extents_info = { 
        .inode_number = inode_number, 
        .max_num_extents = MAX_NUM_EXTENTS,
        .result_num_extents = 0,
        .extents = extents
    };
    if (ioctl(fd, EXT4_IOC_GET_INODE_EXTENTS, &extents_info) < 0) {
        perror("ioctl");
    }

    printf("Fetched %u extents for inode %lu\n", 
        extents_info.result_num_extents,
        extents_info.inode_number);
    for (int i = 0; i < extents_info.result_num_extents; i++) {
        printf("(%lu, %u)\n", 
            extents_info.extents[i].start_block,
            extents_info.extents[i].length
        );
    }
    
    return 0;
}