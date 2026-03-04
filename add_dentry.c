#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>

#define EXT4_NAME_LEN 255

struct ext4_evfs_dentry_add {
    uint64_t parent_inode_number;
    uint64_t child_inode_number;
    uint8_t file_type;
    char name[EXT4_NAME_LEN];
};

#define EXT4_IOC_ADD_DENTRY _IOW('f', 101, struct ext4_evfs_dentry_add)
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2

int main() {
    int fd;
    struct ext4_evfs_dentry_add add_info;

    fd = open("/home/evie/code/evfs-sandbox/fileA", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // populate struct
    add_info.parent_inode_number = 2;
    add_info.child_inode_number = 67;
    add_info.file_type = 3;
    char * file_name = "bobert";
    
    strncpy(add_info.name, file_name, sizeof(add_info.name)-1);
    add_info.name[sizeof(add_info.name)-1] = '\0';

    // call ioctl
    if (ioctl(fd, EXT4_IOC_ADD_DENTRY, &add_info) < 0) {
        perror("ioctl ADD_DENTRY");
        close(fd);
        return 1;
    }

    printf("Successfully added directory entry:\n");
    printf("  Parent inode: %llu\n", (unsigned long long)add_info.parent_inode_number);
    printf("  Child inode:  %llu\n", (unsigned long long)add_info.child_inode_number);
    printf("  Name:         '%s'\n", add_info.name);

    close(fd);
    return 0;
}