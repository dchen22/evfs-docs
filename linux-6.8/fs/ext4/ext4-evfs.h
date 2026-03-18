#include "ext4.h"

struct ext4_evfs_add_dentry {
    __u64 parent_inode_number;
    __u64 child_inode_number;
    __u8 file_type;
    char name[EXT4_NAME_LEN];   // EXT4_NAME_LEN = 255
};

struct ext4_evfs_read_dentry {
    __u64 dir_inode_number; // inode to read dentries of
    __u32 target_dentry_index;     // ith dentry to get

    // output fields of returned dentry
    __u32 inode_number;     // inode num 
    __u8 file_type;
    __u8 name_len;
    __u16 _padding;
    char name[EXT4_NAME_LEN];
};

struct ext4_evfs_delete_dentry {
    __u64 dir_inode_number; // inode to read dentries of
    char name[EXT4_NAME_LEN];     // name of dentry to delete
};

struct ext4_evfs_update_dentry {
    __u64 dir_inode_number;
    __u32 target_dentry_index;
    __u32 new_inode_number;
};

#define EXT4_IOC32_PRINTHELLO	_IO('f', 99)
#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)
#define EXT4_IOC_ADD_DENTRY _IOW('f', 101, struct ext4_evfs_add_dentry)
#define EXT4_IOC_READ_DENTRY _IOWR('f', 102, struct ext4_evfs_read_dentry)
#define EXT4_IOC_DELETE_DENTRY _IOWR('f', 103, struct ext4_evfs_delete_dentry)
#define EXT4_IOC_UPDATE_DENTRY _IOWR('f', 104, struct ext4_evfs_update_dentry)

long __ext4_evfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);