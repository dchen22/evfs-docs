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

struct ext4_evfs_iter_inode {
    __u64 start_inode_number;    // starting inode to iterate from
    __u64 result_inode_number;   // next active inode number
};

struct ext4_evfs_iter_freespace {
    __u64 start_block;      // some block in data section ("start block")
    __u64 result_block;     // next free block after start block
    __u64 result_length;    // how many free blocks occur consecutively
};

struct ext4_evfs_extent {
    __u64 start_block;   // physical block number where this extent starts
    __u32 length;        // length in blocks
};

struct ext4_evfs_get_inode_extents {
    __u64 inode_number;         // input
    __u32 max_num_extents;      // input: capacity of user buffer
    __u32 result_num_extents;   // output: number of extents written to user buffer
    struct ext4_evfs_extent __user *extents;    // input: user buffer
};

#define EXT4_IOC32_PRINTHELLO	_IO('f', 99)
#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)
#define EXT4_IOC_ADD_DENTRY _IOW('f', 101, struct ext4_evfs_add_dentry)
#define EXT4_IOC_READ_DENTRY _IOWR('f', 102, struct ext4_evfs_read_dentry)
#define EXT4_IOC_DELETE_DENTRY _IOWR('f', 103, struct ext4_evfs_delete_dentry)
#define EXT4_IOC_UPDATE_DENTRY _IOWR('f', 104, struct ext4_evfs_update_dentry)
#define EXT4_IOC_ITER_INODE _IOR('f', 105, struct ext4_evfs_iter_inode)
#define EXT4_IOC_ITER_FREESPACE _IOR('f', 106, struct ext4_evfs_iter_freespace)
#define EXT4_IOC_GET_INODE_EXTENTS _IOR('f', 107, struct ext4_evfs_get_inode_extents)

long __ext4_evfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);