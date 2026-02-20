#include "ext4.h"

/**
 * @brief journaling operation function type
 */
typedef int (*ext4_evfs_journal_operation) (struct buffer_head * bh, void * op_args);

/**
 * @brief ext4_evfs_journal arguments
 * @param inode: inode
 * @param type: debugging info for journal operation (here, it's mapping logical
 * block to physical block)
 * @param blocks: number of data change (how many operations that touch bitmap/
 * block/group/cluster/superblock)
 * @param rsv_blocks: extra data change you might touch (worse case: e.g. when you
 * need to split a extent, create a block to hold the extent)
 */
typedef struct __ext4_evfs_journal_args {
    struct super_block * sb;
    struct inode * inode; 
    int type;
    int blocks;
    int rsv_blocks;
} ext4_evfs_journal_args;

/**
 * @brief journal operation binding
 * @param op journal operation function
 * @param bh buffer head that is going to be changed during the operation
 * @param op_args extra arguments
 */
typedef struct __ext4_evfs_jop_binding {
    ext4_evfs_journal_operation op;
    struct buffer_head * bh;
    void * op_args;
} ext4_evfs_jop_binding;

// define a function prototype here
int ext4_evfs_journal(ext4_evfs_journal_args * args, ext4_evfs_jop_binding * jopb, int len);
