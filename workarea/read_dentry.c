#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>

struct ext4_evfs_read_dentry {
    uint64_t dir_inode_number; // inode to read dentries of
    uint32_t target_dentry_index;     // ith dentry to get

    // output fields of returned dentry
    uint32_t inode_number;     // inode num 
    uint8_t file_type;
    uint8_t name_len;
    uint16_t _padding;
    char name[256]; // 256 = EXT4_NAME_LEN + 1
};

#define EXT4_IOC_READ_DENTRY _IOWR('f', 102, struct ext4_evfs_read_dentry)

int main(int argc, char * argv[]) {
    if (argc < 3) {
        return 1;
    }
    int fd;
    struct ext4_evfs_read_dentry read_info;

    // zero out read_info
    memset(&read_info, 0, sizeof(read_info));

    fd = open("/home/evie/code/evfs-sandbox/fileA", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    read_info.dir_inode_number = atoi(argv[1]);
    read_info.target_dentry_index = atoi(argv[2]);

    if (ioctl(fd, EXT4_IOC_READ_DENTRY, &read_info) < 0) {
        perror("ioctl READ_DENTRY");
        close(fd);
        return 1;
    }

    printf("Reading inode %lu directory entry %u\n", 
        read_info.dir_inode_number, read_info.target_dentry_index);
    printf("  Inode:     %u\n", read_info.inode_number);
    printf("  Name:      '%s'\n", read_info.name);
    printf("  Type:      %u\n", read_info.file_type);
    printf("  Name len:  %u\n", read_info.name_len);
}