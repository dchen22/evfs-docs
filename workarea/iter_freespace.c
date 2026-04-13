#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>

struct ext4_evfs_iter_freespace {
    uint64_t start_block;      // some block in data section ("start block")
    uint64_t result_block;     // next free block after start block
    uint64_t result_length;    // how many free blocks occur consecutively
};

#define EXT4_IOC_ITER_FREESPACE _IOR('f', 106, struct ext4_evfs_iter_freespace)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ./iter_freespace [starting block num]");
        return 1;
    }
    int fd;
    fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    int start_block = strtoul(argv[1], NULL, 10);

    struct ext4_evfs_iter_freespace iter_info = { 
        .start_block = start_block 
    };
    if (ioctl(fd, EXT4_IOC_ITER_FREESPACE, &iter_info) < 0) {
        perror("ioctl");
    }
    printf("Next available extent start:  %lu\n", iter_info.result_block);
    printf("Next available extent length: %lu\n", iter_info.result_length);
    
    return 0;
}