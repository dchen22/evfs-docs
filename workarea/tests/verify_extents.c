#define _GNU_SOURCE
#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define BLOCK_SIZE 4096
#define EXT4_IOC_GET_INODE_EXTENTS _IOR('f', 107, struct ext4_evfs_get_inode_extents)

int main(int argc, char *argv[]) {
    int fd;
    int bd_fd;
    int inode_number;
    struct ext4_evfs_extent extents[MAX_NUM_EXTENTS];
    struct ext4_evfs_get_inode_extents extents_info = {0};
    void *buf;

    if (argc < 2) {
        printf("Usage: verify_extents [inode num]");
        return 1;
    }
    
    fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /* Open block device for reading */
    bd_fd = open("/dev/sda2", O_RDONLY | O_DIRECT);
    if (bd_fd < 0) {
        perror("open block device");
        return 1;
    }

    inode_number = strtoul(argv[1], NULL, 10);
    
    memset(extents, 0, sizeof(extents));
     
    extents_info.inode_number = inode_number; 
    extents_info.max_num_extents = MAX_NUM_EXTENTS;   /* But we only read one for now */
    extents_info.result_num_extents = 0;
    extents_info.extents = extents;

    while (1) {
        off_t byte_offset;
        ssize_t bytes_read;
        int corrupt;

        /* Get extents */
        if (ioctl(fd, EXT4_IOC_GET_INODE_EXTENTS, &extents_info) < 0) {
            perror("ioctl");
        }

        if (extents_info.result_num_extents == 0) {
            perror("Inode did not return any extents\n");
            return 1;
        }

        if (extents_info.result_num_extents != 1) {
            perror("Inode returned more than one extent, but should've been created with one only\n");
            return 1;
        }

        /* Read first 20 bytes of extent */
        if (posix_memalign(&buf, BLOCK_SIZE, BLOCK_SIZE) != 0) {
            perror("posix_memalign");
            return 1;
        }
        byte_offset = (off_t)extents[0].start_block * BLOCK_SIZE;
        bytes_read = pread(bd_fd, buf, BLOCK_SIZE, byte_offset);
        if (bytes_read < 0) {
            perror("pread");
            free(buf);
            return 1;
        }
        printf("extent[:20] = ");
        for (int i = 0; i < 20; i++) {
            printf("%02x ", ((unsigned char *)buf)[i]);
        }
        printf("\n");

        /* Verify data is 0xAB */
        corrupt = 0;
        for (int i = 0; i < BLOCK_SIZE; i++) {
            if (((unsigned char *)buf)[i] != 0xAB) {
                printf("CORRUPT at byte %d: expected 0xAB got 0x%02x\n", i, ((unsigned char *)buf)[i]);
                corrupt = 1;
                break;
            }
        }
        if (!corrupt) printf("OK\n");

        free(buf);
    }

    
    
    return 0;
}