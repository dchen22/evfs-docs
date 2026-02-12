#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)

int main(void) {
    int fd = open("/home/evie/code/evfs-sandbox/fileA", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    uint64_t block_number = 1000;
    int ret = ioctl(fd, EXT4_IOC_FLIP_BLOCK_BIT, &block_number);
    if (ret < 0) { perror("ioctl"); return 1; }

    printf("flipped block %lu\n", block_number);

    return 0;
}