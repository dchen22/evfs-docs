#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ./flip_block_bit [block num]");
        return 1;
    }
    int fd;
    fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    uint64_t block_number = strtoul(argv[1], NULL, 10);
    
    if (ioctl(fd, EXT4_IOC_FLIP_BLOCK_BIT, &block_number) < 0) {
        perror("ioctl");
        return 1;
    }
    
    return 0;
}