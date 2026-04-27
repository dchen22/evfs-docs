#define _GNU_SOURCE
#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ext4_evfs_extent {
    uint64_t start_block;   // physical block number where this extent starts
    uint32_t length;        // length in blocks
};

struct ext4_evfs_inode_remap {
    uint64_t inode_number;                         // input
    struct ext4_evfs_extent *extents;    // input: array of new extents
    uint32_t num_extents;                          // input: len(extents)
};

struct ext4_evfs_iter_freespace {
    uint64_t start_block;      // some block in data section ("start block")
    uint64_t result_block;     // next free block after start block
    uint64_t result_length;    // how many free blocks occur consecutively
};

#define BLOCK_SIZE 4096
#define EXT4_IOC_ITER_FREESPACE _IOR('f', 106, struct ext4_evfs_iter_freespace)
#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)
#define EXT4_IOC_INODE_REMAP _IOW('f', 108, struct ext4_evfs_inode_remap)

int main(int argc, char* argv[]) {
    int fd = -1;
    int bd_fd = -1;
    struct ext4_evfs_extent *extents;
    struct ext4_evfs_inode_remap remap_info = {0};
    struct ext4_evfs_iter_freespace iter_freespace_info = {0};
    int err = 1;

    if (argc < 2) {
        printf("Usage: loop_remap [inode number]\n");
        goto cleanup;
    }


    fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) {
        perror("open");
        goto cleanup;
    }

    /* Open block device for writing to extent */
    bd_fd = open("/dev/sda2", O_WRONLY | O_DIRECT);
    if (bd_fd < 0) {
        perror("open block device");
        goto cleanup;
    }

    /* Allocate reusable space for extent */
    extents = malloc(sizeof(struct ext4_evfs_extent));
    if (extents == NULL) {
        printf("malloc failed\n");
        goto cleanup;
    }

    /* Initialize remapping info */
    remap_info.inode_number = strtoul(argv[1], NULL, 10);
    remap_info.num_extents = 1;
    remap_info.extents = extents;

    while (1) {
        off_t byte_offset;
        size_t write_size;
        ssize_t bytes_written;
        void *buf;

        /* Randomly find free space */
        iter_freespace_info.start_block = rand();
        if (ioctl(fd, EXT4_IOC_ITER_FREESPACE, &iter_freespace_info) < 0) {
            perror("ioctl");
            goto cleanup;
        }
        if (iter_freespace_info.result_block == 0) {
            continue;
        }

        /* Claim the discovered free space */
        if (ioctl(fd, EXT4_IOC_FLIP_BLOCK_BIT, &(iter_freespace_info.result_block)) < 0) {
            perror("ioctl");
            goto cleanup;
        }

        /* Write stuff to the discovered free space */
        byte_offset = (off_t)iter_freespace_info.result_block * BLOCK_SIZE;
        write_size = BLOCK_SIZE;
        if (posix_memalign(&buf, BLOCK_SIZE, write_size) != 0) {
            perror("posix_memalign");
            // free(buf);   // buf not allocated because posix memalign failure
            goto cleanup;
        }
        memset(buf, 0xAB, write_size);
        bytes_written = pwrite(bd_fd, buf, write_size, byte_offset);
        if (bytes_written < 0) {
            perror("pwrite");
            free(buf);
            goto cleanup;
        }

        /* Remap inode */
        (remap_info.extents)[0].start_block = iter_freespace_info.result_block;
        (remap_info.extents)[0].length = 1;
        if (ioctl(fd, EXT4_IOC_INODE_REMAP, &remap_info) < 0) {
            perror("ioctl");
            free(buf);
            goto cleanup;
        }

        printf("Remapped to block %lu\n", iter_freespace_info.result_block);

        free(buf);
    }


    err = 0;
cleanup:
    if (extents != NULL) { free(extents); }

    if (bd_fd >= 0) { close(bd_fd); }

    if (fd >= 0) { close(fd); }

    return 0;
}