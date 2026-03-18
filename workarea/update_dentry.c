#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>

struct ext4_evfs_update_dentry {
    uint64_t dir_inode_number;
    uint32_t target_dentry_index;
    uint32_t new_inode_number;
};

#define EXT4_IOC_UPDATE_DENTRY _IOWR('f', 104, struct ext4_evfs_update_dentry)

int main(int argc, char * argv[]) {
    int fd;
    struct ext4_evfs_update_dentry update_info;

    memset(&update_info, 0, sizeof(update_info));

    if (argc != 4) {
        fprintf(stderr, "ext4 update-dentry usage: ./update_dentry [directory inode num] [dentry index] [new inode num]\n");
        return 1;
    }

    fd = open("/home/evie/code/evfs-sandbox/fileA", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    update_info.dir_inode_number = strtoull(argv[1], NULL, 10);
    update_info.target_dentry_index = strtoull(argv[2], NULL, 10);
    update_info.new_inode_number = strtoull(argv[3], NULL, 10);

    if (ioctl(fd, EXT4_IOC_UPDATE_DENTRY, &update_info) < 0) {
        perror("ioctl update_DENTRY");
        close(fd);
        return 1;
    }

    printf("updated directory inode %lu dentry index %u to inode num = %u\n",
        update_info.dir_inode_number, update_info.target_dentry_index,
        update_info.new_inode_number);

    close(fd);
    return 0;
}