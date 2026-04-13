#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>

struct ext4_evfs_iter_inode {
    uint64_t start_inode_number;    // starting inode to iterate from
    uint64_t result_inode_number;   // next active inode number
};

#define EXT4_IOC_ITER_INODE _IOR('f', 105, struct ext4_evfs_iter_inode)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ./iter_inode [starting inode num]");
        return 1;
    }
    int fd;
    fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    int start_inode_number = strtoul(argv[1], NULL, 10);

    struct ext4_evfs_iter_inode iter_info = { 
        .start_inode_number = start_inode_number 
    };
    if (ioctl(fd, EXT4_IOC_ITER_INODE, &iter_info) < 0) {
        perror("ioctl");
    }
    printf("Next in-use inode: %lu\n", iter_info.result_inode_number);
    
    return 0;
}