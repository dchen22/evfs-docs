#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: test_data_bitflip [block number]\n");
    }
    int fd = open("/home/evie/code/evfs-sandbox/fileA", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    uint64_t block_number = strtoul(argv[1], NULL, 10);
    int ret = ioctl(fd, EXT4_IOC_FLIP_BLOCK_BIT, &block_number);
    if (ret < 0) { perror("ioctl"); return 1; }

    printf("flipped block %lu\n", block_number);

    return 0;
}