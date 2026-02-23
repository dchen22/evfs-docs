#include "ext4.h"

struct ext4_evfs_dentry_add {
    __u64 parent_inode_number;
    __u64 child_inode_number;
    __u8 file_type;
    char name[EXT4_NAME_LEN];
};

struct ext4_evfs_dentry_read {
    __u64 dir_inode_number;
    char buffer[4096];
};

#define EXT4_IOC32_PRINTHELLO	_IO('f', 99)
#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)
#define EXT4_IOC_ADD_DENTRY _IOW('f', 101, struct ext4_evfs_dentry_add)
#define EXT4_IOC_READ_DENTRY _IOWR('f', 102, struct ext4_evfs_dentry_read)

long __ext4_evfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);