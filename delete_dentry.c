#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>

#define EXT4_NAME_LEN 255

struct ext4_evfs_delete_dentry {
    uint64_t dir_inode_number; // inode to read dentries of
    uint32_t target_dentry_index;     // ith dentry to get
};

#define EXT4_IOC_DELETE_DENTRY _IOWR('f', 103, struct ext4_evfs_delete_dentry)

int main(int argc, char * argv[]) {
    int fd;
    struct ext4_evfs_delete_dentry delete_info;

    memset(&delete_info, 0, sizeof(delete_info));

    if (argc != 3) {
        fprintf(stderr, "ext4 delete-dentry usage: ./delete_dentry [directory inode num] [dentry index]\n");
        return 1;
    }

    fd = open("/home/evie/code/evfs-sandbox/fileA", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    delete_info.dir_inode_number = strtoull(argv[1], NULL, 10);
    delete_info.target_dentry_index = strtoull(argv[2], NULL, 10);

    if (ioctl(fd, EXT4_IOC_DELETE_DENTRY, &delete_info) < 0) {
        perror("ioctl DELETE_DENTRY");
        close(fd);
        return 1;
    }

    printf("Deleted dentry index %u from directory inode %lu\n",
        delete_info.target_dentry_index, delete_info.dir_inode_number);

    close(fd);
    return 0;
}