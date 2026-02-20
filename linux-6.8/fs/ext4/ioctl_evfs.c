#include <linux/types.h>
#include "evfs.h"
#include "ioctl_evfs.h"
#include "ext4.h"
#include "ext4_jbd2.h"

// static helper function declaration
static long ext4_evfs_hello(void);
static int ext4_evfs_block_alloc_meta(struct buffer_head * bh, void * op_args);
static int ext4_evfs_block_alloc_set_bit(struct buffer_head * bh, void * op_args);
static int ext4_evfs_block_alloc_meta(struct buffer_head * bh, void * op_args);
static long ext4_evfs_block_alloc(struct inode * ino, struct super_block * sb, ext4_fsblk_t bindex);


/**
 * @brief hello from evfs
 */
static long ext4_evfs_hello() {
    printk(KERN_INFO "Hello, bad apple!\n");
    return 0;
}

// ext4_evfs_block_alloc

/**
 * @brief ext4_evfs_block_alloc_set_bit arguments
 * @param index block relative index
 */
typedef struct __ext4_evfs_block_alloc_set_bit_args {
    int index; // TODO: confirm index is of integer from the caller
} ext4_evfs_block_allow_set_bit_args;

/**
 * @brief helper: turn a block bit to 1
 * @param bh buffer head of the group descriptor
 * @param op_args extra arguments, see ext4_evfs_block_alloc_meta_args
 */
static int ext4_evfs_block_alloc_set_bit(struct buffer_head * bh, void * op_args) {
    ext4_evfs_block_allow_set_bit_args * args = (ext4_evfs_block_allow_set_bit_args *) op_args;
    if (ext4_test_and_set_bit(args->index, bh->b_data)) {
        return -EEXIST;
    }

    return 0;
}

/**
 * @brief ext4_evfs_block_alloc_meta arguments
 * @param sb super block
 * @param index block group index
 * @param desc group descriptor
 */
typedef struct __ext4_evfs_block_alloc_meta_args {
    struct super_block * sb;
    ext4_group_t index;
    struct ext4_group_desc * desc;
} ext4_evfs_block_alloc_meta_args;

/**
 * @brief helper: change group descriptor metadata
 * @param bh buffer head of the group descriptor
 * @param op_args extra arguments, see ext4_evfs_block_alloc_meta_args
 */
static int ext4_evfs_block_alloc_meta(struct buffer_head * bh, void * op_args) {
    ext4_evfs_block_alloc_meta_args * args = (ext4_evfs_block_alloc_meta_args *) op_args;
    // modify the free block count
    u32 free_blocks_count = args->desc->bg_free_blocks_count_hi << 16 | args->desc->bg_free_blocks_count_lo;
    free_blocks_count -= 1;
    args->desc->bg_free_blocks_count_hi = (u16) (free_blocks_count >> 16);
    args->desc->bg_free_blocks_count_lo = (u16) (free_blocks_count & ~(0xFFFF << 16));

    // modify the checksum
    ext4_block_bitmap_csum_set(args->sb, args->desc, bh);
    ext4_group_desc_csum_set(args->sb, args->index, args->desc);

    return 0;
}

/**
 * @brief journal a block allocation (?to an extent).
 * Note that this function alone is now creating a temporary
 * inconsistency because there's a leak block (i.e. not
 * being used by any inode)
 * 
 * WARNING: TESTING ONLY; Do NOT use this on a system fs.
 * 
 * @param ino pointer to the in-memory the file inode
 * @param sb pointer to the in-memory superblock
 * @param bindex the index of block to allocate
 * @return 0 on success otherwise error code:
 *   * -EEXIST: already allocated;
 *   * -EINVAL: fail to load group descriptor. either rcu deference fails
 *     or group index out of range;
 *   * -ERRNO: errors from ext4 functions;
 */
static long ext4_evfs_block_alloc(struct inode * ino, struct super_block * sb, ext4_fsblk_t bindex) {
    // get the group number and block offset
    ext4_group_t grp_i;
    ext4_grpblk_t offset;
    ext4_get_group_no_and_offset(sb, bindex, &grp_i, &offset);

    // read the block bitmap
    ext4_evfs_jop_binding jobp[2];
    jobp[0].bh = ext4_read_block_bitmap(sb, grp_i);
    if (IS_ERR(jobp[0].bh)) {
        brelse(jobp[0].bh); // release the buffer head
        return PTR_ERR(jobp[0].bh); // error from ext4_read_block_bitmap
    }

    // read the group descriptor
    struct ext4_group_desc * gdp = ext4_get_group_desc(sb, grp_i, &(jobp[1].bh));
    if (!gdp) {
        brelse(jobp[0].bh);
        brelse(jobp[1].bh);
        return -EINVAL;
    }

    ext4_evfs_block_allow_set_bit_args set_bit_args;
    set_bit_args.index = offset;

    ext4_evfs_block_alloc_meta_args meta_args;
    meta_args.sb = sb;
    meta_args.index = grp_i;
    meta_args.desc = gdp;

    jobp[0].op = ext4_evfs_block_alloc_set_bit;
    jobp[0].op_args = &set_bit_args;

    jobp[1].op = ext4_evfs_block_alloc_meta;
    jobp[1].op_args = &meta_args;

    ext4_evfs_journal_args args;
    args.sb = sb;
    args.inode = ino;
    args.type = EXT4_HT_MAP_BLOCKS;
    args.blocks = 2;
    args.rsv_blocks = 0;

    int ret = ext4_evfs_journal(&args, jobp, 2);

    brelse(jobp[0].bh);
    brelse(jobp[1].bh);
    return (long) ret;
}

long ext4_evfs_entry(unsigned int cmd, struct inode * ino, struct super_block * sb, unsigned long arg) {
    switch(cmd) {
        case EXT4_IOC_EVFS_HELLO:
            return ext4_evfs_hello();
        case EXT4_IOC_EVFS_BLK_ALLOC:
            return ext4_evfs_block_alloc(ino, sb, (ext4_fsblk_t) arg);
        case EXT4_IOC_EVFS_INO_ALLOC:
            // TODO: uncomment this when done
            // return ext4_evfs_inode_alloc(ino, sb, (ino_t) arg);
            return 0;
        default:
            return -ENOTTY;
    }
}