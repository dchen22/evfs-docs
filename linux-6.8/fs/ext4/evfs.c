/**
 * evfs interface for EXT4
 */

// TODO: increment i_version in any evfs functions that
// ----: modify inode metadata
// TODO: add block tracker, similar to how we track inode

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/quotaops.h>
#include <linux/iversion.h>
#include "ext4.h"
#include "ext4_jbd2.h"
#include "ext4_extents.h"
#include "evfs.h"

static long __evfs_inode_dec_nlink(handle_t * h, struct inode * ino)
{
	drop_nlink(ino);
	return ext4_mark_inode_dirty(h, ino);
}

static long __evfs_inode_inc_nlink(handle_t * h, struct inode * ino)
{
	inc_nlink(ino);
	return ext4_mark_inode_dirty(h, ino);
}

// helper functions
static long __evfs_inode_track(struct inode * ino)
{
	int ret;
	struct ext4_sb_info *sbi = EXT4_SB(ino->i_sb);	
	evfs_bitop ebitop;
	ebitop.type = EBO_BOOL;
	ebitop.op_b = test_and_set_bit;
 
	if ((ret = evfs_bit_wrapper(sbi->s_inode_tracker, &ebitop, ino->i_ino)) == 1) {
		ret = -EEXIST;
	}
	return ret;
}

// untrack using test and clear
static long __evfs_inode_untrack(struct inode * ino)
{
	int ret;
	struct ext4_sb_info *sbi = EXT4_SB(ino->i_sb);
	evfs_bitop ebitop;
	ebitop.type = EBO_BOOL;
	ebitop.op_b = test_and_clear_bit;

	if ((ret = evfs_bit_wrapper(sbi->s_inode_tracker, &ebitop, ino->i_ino)) == 0) {
		ret = -ENOENT; 
	}
	
	if (ret == 1) ret = 0;
	return ret;
}

// untrack using clear
static long __evfs_inode_untrack_nolock(struct inode * ino)
{
	struct ext4_sb_info *sbi = EXT4_SB(ino->i_sb);
	evfs_bitop ebitop;
	ebitop.type = EBO_VOID;
	ebitop.op_v = clear_bit;
	return evfs_bit_wrapper(sbi->s_inode_tracker, &ebitop, ino->i_ino);
}

/**
 * @breif helper to swap extent
 * PRE-COND:
 * - curr extent contains the extent to process
 *
 * POST-COND:
 * - curr extent contains the valid logical range and the
 *   physical blocks that are swapped out
 * - next extent is updated to the next range to iterate
 */
static long __evfs_extent_map(handle_t * handle, struct inode * inode,
				struct ext4_evfs_ext * curr, struct ext4_evfs_ext * next)
{
	int ret = 0, err = 0, eof = 1;
	struct ext4_ext_path * path = NULL;
	ext4_lblk_t lblk = curr->log_start;
	unsigned int len = curr->len;

	// invalidate extent status tree
	ext4_es_remove_extent(inode, lblk, len);
	bool done = false;
	while (!done) {
		path = ext4_find_extent(inode, lblk, NULL, EXT4_EX_NOCACHE);
		if (IS_ERR(path)) {
			ret = PTR_ERR(path);
			goto finish;
		}

		struct ext4_extent * ex = path[path->p_depth].p_ext;
		if (!ex) {
			ret = eof;
			goto finish;
		}

		// NOTE: volatile is used for debugging
		volatile ext4_lblk_t e_blk = le32_to_cpu(ex->ee_block);
		volatile unsigned int e_len = ext4_ext_get_actual_len(ex);

		// DONE: handle holes
		// if before the hole, move the cursor to the current
		// hole, if the cursor is after the hole, move the cursor
		// to the next extent.
		if (lblk < e_blk) {
			lblk = e_blk;
		} else if (lblk >= e_blk + e_len) {
			lblk = ext4_ext_next_allocated_block(path);
			if (lblk >= EXT_MAX_BLOCKS) {
				ret = eof;
				goto finish;
			}

			goto cont;
		}

		// get to the next length
		len = curr->log_start + len - lblk;

		// DONE: split bc the ext physical blk can be in other state
		// ----: (e.g. uninitialized), or the next ext physical blk is
		// ----: is not in the range of the curr physical blocks (very
		// ----: likely)
		// check where the left and right boundary is and initiate
		// the split; if split, find the extent again.
		if (e_blk < lblk) {
			if ((err = ext4_force_split_extent_at(handle, inode, &path, lblk, 0))) {
				ret = err;
				goto finish;
			}

			goto cont;
		}

		// DEBUG: assert(lblk == e_blk);
		// the specified extent endpoint goes beyond current extent
		if (e_blk + e_len < lblk + len) len = e_blk + e_len - lblk;

		// the specified extent endpoint doesn't cover current extent
		if (e_blk + e_len > lblk + len) {
			if ((err = ext4_force_split_extent_at(handle, inode, &path, lblk + len, 0))) {
				ret = err;
				goto finish;
			}

			goto cont;
		}

		// DONE: set the new physical start
		unsigned long long tmp = ext4_ext_pblock(ex);
		ext4_ext_store_pblock(ex, curr->phy_start + lblk - curr->log_start);

		// DONE: try merging the extent
		ext4_ext_try_to_merge(handle, inode, path, ex);

		// DONE: mark the extent as dirty
		if ((err = ext4_ext_dirty(handle, inode, &(path[path->p_depth])))) {
			ret = err;
			goto finish;
		}

		goto set_next;

		set_next:
		if (lblk + curr->len > e_blk + e_len) {
			// specified extent endpoint goes beyond current
			// extent endpoint
			next->phy_start = curr->phy_start + e_len;
			next->log_start = e_blk + e_len;
			next->len = curr->log_start + curr->len - next->log_start;
		} else {
			finish:
			memset(next, 0, sizeof(struct ext4_evfs_ext));
		}

		// complete the swap
		if (ret == 0 && err == 0) {
			curr->phy_start = tmp;
			curr->log_start = lblk;
			curr->len = len;
		}

		done = true;

		cont:
		ext4_free_ext_path(path);
		path = NULL;
	}

	return (long) ret;
}

// core evfs functions
long evfs_hello(void);
long evfs_iver(struct inode *, unsigned long long *);
long evfs_block_alloc(struct inode *, struct super_block *, ext4_fsblk_t);
long evfs_block_free(struct inode *, struct super_block *, ext4_fsblk_t);
long evfs_fspace_iter(struct super_block *, struct ext4_evfs_fsp_iter_args *);
long evfs_inode_read(struct super_block *, struct evfs_ino_read_args *); 
long evfs_inode_alloc(struct super_block *, ino_t);
long evfs_inode_free(struct super_block *, ino_t);
long evfs_inode_iter(struct super_block *, struct ext4_evfs_ino_iter_args *);
long evfs_dentry_add(struct super_block *, struct ext4_evfs_de_add_args *);
long evfs_dentry_read(struct super_block *, struct ext4_evfs_de_read_args *);
long evfs_dentry_delete(struct super_block *, struct ext4_evfs_de_delete_args *);
long evfs_dentry_update(struct super_block *, struct ext4_evfs_de_update_args *);
long evfs_extent_read(struct super_block *, struct ext4_evfs_ext_read_args *);
long evfs_extent_move(struct super_block *, struct ext4_evfs_ext_mv_args *);

/**
 * @brief hello from evfs
 */
long evfs_hello()
{
	printk(KERN_INFO "Hello, bad apple!\n");
	return 0;
}

/**
 * @brief get inode version
 */
long evfs_iver(struct inode * ino, unsigned long long * ver)
{
	// print some inode info
	unsigned long long kver = atomic64_read(&ino->i_version);
	memcpy(ver, &kver, sizeof(unsigned long long));
	return 0;
}

/**
 * @brief journal a block allocation.
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
 *   * -ERRNO: other errors from ext4 functions;
 */
long evfs_block_alloc(struct inode * ino, struct super_block * sb, ext4_fsblk_t bindex)
{
	// get the group number and block offset
	ext4_group_t group;
	ext4_grpblk_t offset;
	ext4_get_group_no_and_offset(sb, bindex, &group, &offset);
	int ret = 0, err = 0;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;

	// read the block bitmap
	struct buffer_head * bitmap_bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(bitmap_bh)) {
		return PTR_ERR(bitmap_bh); // error from ext4_read_block_bitmap
	}

	// read the group descriptor
	struct buffer_head * grpdsc_bh;
	struct ext4_group_desc * grpdsc = ext4_get_group_desc(sb, group, &grpdsc_bh);
	if (!grpdsc) {
		brelse(bitmap_bh);
	   	return (long) -EINVAL;
	}

	handle_t * handle = ext4_journal_start(ino, EXT4_HT_MAP_BLOCKS, 2);

	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto out;
	}

	if ((err = ext4_journal_get_write_access(handle, sb, bitmap_bh, EXT4_JTR_NONE)) < 0) {
		ret = err;
		goto stop_journal;
	}

	if ((err = ext4_journal_get_write_access(handle, sb, grpdsc_bh, EXT4_JTR_NONE)) < 0) {
		ret = err;
		goto stop_journal;
	}

	ext4_lock_group(sb, group);
	if (ext4_test_and_set_bit(offset, bitmap_bh->b_data)) {
		ret = -EEXIST;
		ext4_unlock_group(sb, group);
		goto stop_journal;
	}

	// decrease free count
	ext4_free_group_clusters_set(sb, grpdsc, EXT4_NUM_B2C(sbi,
		EXT4_C2B(sbi, ext4_free_group_clusters(sb, grpdsc)) - 1));

	if (ext4_has_group_desc_csum(sb)) {
		// initialize the block containing the inode if uninitialized
		if (grpdsc->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
			grpdsc->bg_flags &= cpu_to_le16(~EXT4_BG_BLOCK_UNINIT);
			ext4_free_group_clusters_set(sb, grpdsc, 
				ext4_free_clusters_after_init(sb, group, grpdsc));
		}

		// recompute group descriptor + bitmap checksum
		ext4_block_bitmap_csum_set(sb, grpdsc, bitmap_bh);
		ext4_group_desc_csum_set(sb, group, grpdsc);
	}

	ext4_unlock_group(sb, group);

	// update per-cpu cluster (a group of blocks) counter
	percpu_counter_sub(&sbi->s_freeclusters_counter, EXT4_NUM_B2C(sbi, 1));

	// update superblock free block counter and checksum 
	lock_buffer(sbi->s_sbh);
	ext4_free_blocks_count_set(es, ext4_free_blocks_count(es) - 1);
	ext4_superblock_csum_set(sb);
	unlock_buffer(sbi->s_sbh);

	if ((err = ext4_handle_dirty_metadata(handle, ino, grpdsc_bh)) < 0) {
		ret = err;
		goto stop_journal;
	}

	if ((err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh)) < 0) {
		ret = err;
		goto stop_journal;
	}

stop_journal:
	if ((err = ext4_journal_stop(handle)) < 0) {
		ret = err;
		goto out;
	}

out:
	brelse(bitmap_bh);
	return (long) ret;
}

long evfs_block_free(struct inode * ino, struct super_block * sb, ext4_fsblk_t bindex)
{
	// get the group number and block offset
	ext4_group_t group;
	ext4_grpblk_t offset;
	ext4_get_group_no_and_offset(sb, bindex, &group, &offset);
	int ret = 0, err = 0;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;

	// read the block bitmap
	struct buffer_head * bitmap_bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(bitmap_bh)) {
		return PTR_ERR(bitmap_bh); // error from ext4_read_block_bitmap
	}

	// read the group descriptor
	struct buffer_head * grpdsc_bh;
	struct ext4_group_desc * grpdsc = ext4_get_group_desc(sb, group, &grpdsc_bh);
	if (!grpdsc) {
		brelse(bitmap_bh);
	   	return (long) -EINVAL;
	}

	handle_t * handle = ext4_journal_start(ino, EXT4_HT_MAP_BLOCKS, 2);

	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto out;
	}

	if ((err = ext4_journal_get_write_access(handle, sb, bitmap_bh, EXT4_JTR_NONE)) < 0) {
		ret = err;
		goto stop_journal;
	}

	if ((err = ext4_journal_get_write_access(handle, sb, grpdsc_bh, EXT4_JTR_NONE)) < 0) {
		ret = err;
		goto stop_journal;
	}

	ext4_lock_group(sb, group);
	if (!ext4_test_and_clear_bit(offset, bitmap_bh->b_data)) {
		ret = -EEXIST;
		ext4_unlock_group(sb, group);
		goto stop_journal;
	}

	// increase free count
	ext4_free_group_clusters_set(sb, grpdsc, EXT4_NUM_B2C(sbi,
		EXT4_C2B(sbi, ext4_free_group_clusters(sb, grpdsc)) + 1));

	if (ext4_has_group_desc_csum(sb)) {
		// recompute group descriptor + bitmap checksum
		ext4_block_bitmap_csum_set(sb, grpdsc, bitmap_bh);
		ext4_group_desc_csum_set(sb, group, grpdsc);
	}

	ext4_unlock_group(sb, group);

	// update per-cpu cluster (a group of blocks) counter
	percpu_counter_add(&sbi->s_freeclusters_counter, EXT4_NUM_B2C(sbi, 1));

	// update superblock free block counter and checksum 
	lock_buffer(sbi->s_sbh);
	ext4_free_blocks_count_set(es, ext4_free_blocks_count(es) + 1);
	ext4_superblock_csum_set(sb);
	unlock_buffer(sbi->s_sbh);

	if ((err = ext4_handle_dirty_metadata(handle, ino, grpdsc_bh)) < 0) {
		ret = err;
		goto stop_journal;
	}

	if ((err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh)) < 0) {
		ret = err;
		goto stop_journal;
	}

stop_journal:
	if ((err = ext4_journal_stop(handle)) < 0) {
		ret = err;
		goto out;
	}

out:
	brelse(bitmap_bh);
	return (long) ret;
}

long evfs_fspace_iter(struct super_block * sb, struct ext4_evfs_fsp_iter_args * args)
{
	__u64 found_start = 0;
	__u64 found_length = 0;
	__u64 total_blocks = ext4_blocks_count(EXT4_SB(sb)->s_es);
	int in_free_extent = 0;

	for (__u64 b = args->in.start; b <= total_blocks; b++) {
		ext4_group_t group;
		ext4_grpblk_t offset;
		ext4_get_group_no_and_offset(sb, b, &group, &offset);

		struct buffer_head *bitmap_bh = ext4_read_block_bitmap(sb, group);
		// found unreadable blocks
		if (IS_ERR_OR_NULL(bitmap_bh)) {
			// assume current free extent stops right before unreadable blocks
			if (in_free_extent) break;
			// otherwise, continue checking for free extents after unreadable blocks
			continue;
		}

		int is_free = !ext4_test_bit(offset, bitmap_bh->b_data);
		brelse(bitmap_bh);

		if (is_free) {
			if (!in_free_extent) {
				found_start = b;
				in_free_extent = 1;
			}
			found_length++;
		} else if (in_free_extent) {
			break;
		}
	}

	args->out.block = found_start;
	args->out.length = found_length;
	return 0;
}

/**
 * @brief inode statistics with version
 * This is a kernel helper and/or a function hence we don't copy to
 * user directly here
 */
long evfs_inode_read(struct super_block * sb, struct evfs_ino_read_args * args)
{
	int ret = 0;
	struct inode * ino = ext4_iget(sb, args->in.iindex, EXT4_IGET_NORMAL);
	if (IS_ERR(ino)) {
		ret = PTR_ERR(ino);
		goto out;
	}

	inode_lock_shared(ino);

	evfs_stat stat;
	stat.est_dev = ino->i_sb->s_dev;
	stat.est_ino = ino->i_ino;
	stat.est_mode = ino->i_mode;
	stat.est_nlink = ino->i_nlink;
	stat.est_uid = ino->i_uid.val;
	stat.est_gid = ino->i_gid.val;

	if (S_ISBLK(ino->i_mode) || S_ISCHR(ino->i_mode)) {
		stat.est_rdev = ino->i_rdev;
	}

	stat.est_version = atomic64_read(&ino->i_version);
	stat.est_size = ino->i_size;
	stat.est_blksize = i_blocksize(ino);
	stat.est_blocks = ino->i_blocks;
	stat.est_atime = ino->__i_atime.tv_sec;
	stat.est_atime_nsec = ino->__i_atime.tv_nsec;
	stat.est_mtime = ino->__i_mtime.tv_sec;
	stat.est_mtime_nsec = ino->__i_mtime.tv_nsec;
	stat.est_ctime = ino->__i_ctime.tv_sec;
	stat.est_ctime_nsec = ino->__i_ctime.tv_nsec;

	inode_unlock_shared(ino);

	memcpy(&(args->out), &stat, sizeof(evfs_stat));
	iput(ino);

out:
	return (long) ret; 
}

/** 
 * @brief journal a inode allocation.
 * Note that this function alone is now creating a temporary
 * inconsistency because there's a leak inode (i.e. not
 * being used by any inode)
 * 
 * WARNING: TESTING ONLY; Do NOT use this on a system fs.
 * 
 * @param sb pointer to the in-memory superblock
 * @param iindex the index of inode to allocate
 * @return 0 on success otherwise error code:
 *   * -EEXIST: already allocated;
 *   * -EINVAL: fail to load group descriptor. either rcu deference fails
 *   * -ENOMEM: no enough space to allocate inode in memory
 *     or group index out of range;
 *   * -ERRNO: other errors from ext4 functions;
 */
long evfs_inode_alloc(struct super_block * sb, ino_t iindex)
{
	ext4_group_t group = (iindex - 1) / EXT4_INODES_PER_GROUP(sb);
	ino_t offset = (iindex - 1) % EXT4_INODES_PER_GROUP(sb);
	int ret = 0, err = 0;
	struct buffer_head * inode_bitmap_bh;
	struct buffer_head * block_bitmap_bh;
	struct buffer_head * grpdsc_bh;
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	inode_bitmap_bh = ext4_read_inode_bitmap(sb, group);
	if (IS_ERR(inode_bitmap_bh)) {
		return PTR_ERR(inode_bitmap_bh); // error from ext4_read_inode_bitmap
	}

	block_bitmap_bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(block_bitmap_bh)) {
		brelse(inode_bitmap_bh);
		return PTR_ERR(block_bitmap_bh); // error from ext4_read_block_bitmap
	}

	// read the group descriptor
	struct ext4_group_desc * grpdsc = ext4_get_group_desc(sb, group, &grpdsc_bh);

	if (!grpdsc) {
		ret = -EINVAL;
		goto out;
	}

	// create the inode
	struct inode * ino = new_inode(sb);
	if (!ino) {
		ret = -ENOMEM;
		goto out;
	}

	if ((err = dquot_initialize(ino))) {
		ret = err;
		goto fail_init;
	}

	handle_t * handle = ext4_journal_start(ino, EXT4_HT_MAP_BLOCKS, 5);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto fail_init;
	}

	if ((err = ext4_journal_get_write_access(handle, sb, inode_bitmap_bh, EXT4_JTR_NONE)) < 0) {
		ret = err;
		goto stop_journal;
	}
	
	if ((err = ext4_journal_get_write_access(handle, sb, block_bitmap_bh, EXT4_JTR_NONE)) < 0) {
		ret = err;
		goto stop_journal;
	}
	
	if ((err = ext4_journal_get_write_access(handle, sb, grpdsc_bh, EXT4_JTR_NONE)) < 0) {
		ret = err;
		goto stop_journal;
	}

	ext4_lock_group(sb, group);
	if (ext4_test_and_set_bit(offset, inode_bitmap_bh->b_data)) {
		ret = -EEXIST;
		ext4_unlock_group(sb, group);
		goto stop_journal;
	}
	ext4_unlock_group(sb, group);

	// track the inode
	ino->i_ino = iindex;
	if ((err = __evfs_inode_track(ino)) < 0) {
		ret = err;
		goto stop_journal;
	}

	struct ext4_inode_info *ei = EXT4_I(ino);

	ino->i_mode = S_IFREG | 0644; // use file for now, may change it later
	ino->i_blocks = 0; // optimal IO size (for stat), not the fs block size; refer to ialloc.c:1253
	simple_inode_init_ts(ino); // set time
	set_nlink(ino, 1); // since i_count will be 0 eventually, set link count to 1 to make inode persistent
	ei->i_crtime = inode_get_mtime(ino);

	memset(ei->i_data, 0, sizeof(ei->i_data));
	ei->i_dir_start_lookup = 0;
	ei->i_disksize = 0;
	ei->i_flags = 0;
	ei->i_file_acl = 0;
	ei->i_dtime = 0;
	ei->i_block_group = group;
	ei->i_last_alloc_group = ~0;

	ext4_set_inode_flags(ino, true);

	// you must locked a dangling inode here
	if (insert_inode_locked(ino) < 0) {
		ret = -EIO;
		goto stop_journal;
	}

	ino->i_generation = get_random_u32();

	if (ext4_has_metadata_csum(sb)) {
		__le32 inum = cpu_to_le32(ino->i_ino);
		__le32 gen = cpu_to_le32(ino->i_generation);
		__u32 csum = ext4_chksum(sbi, sbi->s_csum_seed, (__u8 *) &inum, sizeof(inum));
		ei->i_csum_seed = ext4_chksum(sbi, csum, (__u8 *) &gen, sizeof(gen));
	}

	ext4_clear_state_flags(ei); /* only relevant on 32-bit archs */
	ext4_set_inode_state(ino, EXT4_STATE_NEW);

	// allocate quota for inode
	if ((err = dquot_alloc_inode(ino))) {
		ret = err;
		goto fail_drop;
	}

	// add inode to orphan list
	// TODO: create a doubly linked list to hold pseudo file/dentry to hold
	// 	 the new inode before making it tmpfile and adding it to orphan list
	/* if ((err = ext4_orphan_add(handle, ino)) < 0) {
		ret = err;
		goto fail_free_drop;
	} */	

	// note that ext4_mark_inode_dirty get write access to journal, 
	// so we do NOT need it here
	if ((err = ext4_mark_inode_dirty(handle, ino))) {
		ret = err;
		goto fail_free_drop;
	}

	// update the metadata in the block group descriptor
	ext4_lock_group(sb, group);
	// modify the free inode count
	ext4_free_inodes_set(sb, grpdsc, ext4_free_inodes_count(sb, grpdsc) - 1);

	if (ext4_has_group_desc_csum(sb)) {
		// initialize the block containing the inode if uninitialized
		if (grpdsc->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
			grpdsc->bg_flags &= cpu_to_le16(~EXT4_BG_BLOCK_UNINIT);
			ext4_free_group_clusters_set(sb, grpdsc,
				ext4_free_clusters_after_init(sb, group, grpdsc));
			ext4_block_bitmap_csum_set(sb, grpdsc, block_bitmap_bh);
		}

		ext4_grpblk_t free = EXT4_INODES_PER_GROUP(sb) - ext4_itable_unused_count(sb, grpdsc);
		if (grpdsc->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
			grpdsc->bg_flags &= cpu_to_le16(~EXT4_BG_INODE_UNINIT);
			free = 0;
		}

		if (offset > free) {
			ext4_itable_unused_set(sb, grpdsc, (EXT4_INODES_PER_GROUP(sb) - offset - 1));
		}

		// recompute group descriptor + bitmap checksum
		ext4_inode_bitmap_csum_set(sb, grpdsc, inode_bitmap_bh, EXT4_INODES_PER_GROUP(sb) / 8);
		ext4_group_desc_csum_set(sb, group, grpdsc);
	}

	ext4_unlock_group(sb, group);

	// decrease free inode count
	percpu_counter_dec(&sbi->s_freeinodes_counter);
	// DONE: there is no superblock free inode counters, so we don't update them

	if ((err = ext4_handle_dirty_metadata(handle, ino, grpdsc_bh)) < 0) {
		ret = err;
		goto fail_free_drop;
	}

	if ((err = ext4_handle_dirty_metadata(handle, NULL, block_bitmap_bh)) < 0) {
		ret = err;
		goto fail_free_drop;
	}

	if ((err = ext4_handle_dirty_metadata(handle, NULL, inode_bitmap_bh)) < 0) {
		ret = err;
		goto fail_free_drop;
	}

	unlock_new_inode(ino);
	goto stop_journal;

fail_free_drop:
	dquot_free_inode(ino);

fail_drop:
	clear_nlink(ino);
	unlock_new_inode(ino);

stop_journal:
	if ((err = ext4_journal_stop(handle)) < 0) ret = err;
	if (ret >= 0) {
		iput(ino);
		goto out;
	}

fail_init:	
	dquot_drop(ino);
	ino->i_flags |= S_NOQUOTA;
	iput(ino);

out:
	brelse(block_bitmap_bh);
	brelse(inode_bitmap_bh);
	return (long) ret;
}

/**
 * @brief help to free an inode
 * Note that this function is only intended to free an inode
 * that is created using evfs_inode_alloc but is never modified
 */
long evfs_inode_free(struct super_block * sb, ino_t iindex)
{
	int ret = 0, err;

	// get the inode
	struct inode * ino = ext4_iget(sb, iindex, EXT4_IGET_NORMAL);
	if (IS_ERR(ino)) {
		ret = PTR_ERR(ino);
		goto out;
	}

	inode_lock(ino);
	handle_t * handle = ext4_journal_start(ino, EXT4_HT_MAP_BLOCKS, 3);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto release_inode;
	}

	if (ino->i_nlink > 1) {
		ret = -EBUSY;
		goto stop_journal;
	}

	// disallow freeing an inode not allocated by evfs
	if ((err = __evfs_inode_untrack(ino)) < 0) {
		ret = err;
		goto stop_journal;
	}

	__evfs_inode_dec_nlink(handle, ino);
	ext4_orphan_add(handle, ino);

stop_journal:
	if ((err = ext4_journal_stop(handle)) < 0) ret = err;

release_inode:
	inode_unlock(ino);
	iput(ino);

out:
	return (long) ret;
}

long evfs_inode_iter(struct super_block * sb, struct ext4_evfs_ino_iter_args * args)
{
	__u32 found = 0;
	__u32 total = le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count);

	for (__u32 i = args->in.start; i <= total; i++) {
		// compute the group and offset of the current inode
		// inodes are 1-indexed
		ext4_group_t group = (i - 1) / EXT4_INODES_PER_GROUP(sb);
		ext4_grpblk_t offset = (i - 1) % EXT4_INODES_PER_GROUP(sb);

		// read inode bitmap for this group
		struct buffer_head *bitmap_bh = ext4_read_inode_bitmap(sb, group);
		if (IS_ERR_OR_NULL(bitmap_bh)) {
			continue;
		}

		int in_use = ext4_test_bit(offset, bitmap_bh->b_data);
		brelse(bitmap_bh);

		if (in_use) {
			found = i;
			break;
		}
	}

	args->out.ino_num = found;
	return 0;
}

long evfs_dentry_add(struct super_block * sb, struct ext4_evfs_de_add_args * args)
{
	struct inode * dir_inode;
	struct inode * child_inode;
	struct dentry * parent_dentry;
	struct dentry * child_dentry;
	struct qstr qname;
	int ret = 0, err;

	size_t name_len = strlen(args->in.name);
	if (name_len == 0 || name_len > EXT4_NAME_LEN) {
		return -EINVAL;
	}

	// get parent directory inode
	dir_inode = ext4_iget(sb, args->in.parent_ino_num, EXT4_IGET_NORMAL);
	if (IS_ERR(dir_inode)) {
		return PTR_ERR(dir_inode);
	}

	inode_lock(dir_inode);

	// ensure parent is a dir
	if (!S_ISDIR(dir_inode->i_mode)) {
		ret = -ENOTDIR;
		goto release_pino;
	}

	// look up parent dentry
	parent_dentry = d_find_any_alias(dir_inode);
	if (!parent_dentry) {
		ret = -ENOENT;
		goto release_pino;
	}

	// get child inode (this must exist)
	child_inode = ext4_iget(sb, args->in.child_ino_num, EXT4_IGET_NORMAL);
	if (IS_ERR(child_inode)) {
		ret = PTR_ERR(child_inode);
		goto release_pde;
	}

	inode_lock(child_inode);

	// create dentry for new entry
	qname.name = args->in.name;
	qname.len = strlen(args->in.name);
	qname.hash = full_name_hash(parent_dentry, qname.name, qname.len);

	if (IS_CASEFOLDED(dir_inode)) {
		sb->s_d_op->d_hash(parent_dentry, &qname);
	}

	child_dentry = d_alloc(parent_dentry, &qname);
	if (!child_dentry) {
		ret = -ENOMEM;
		goto release_cino;
	}

	// initialize disk quotas for dir to make sure fs
	// with disk quota enabled doesn't run out of
	// journal credits
	if ((err = dquot_initialize(dir_inode))) {
		ret = err;
		goto release_cde;
	}
	
	if ((err = __ext4_link(dir_inode, child_inode, child_dentry))) {
		ret = err;
	}

release_cde:
	dput(child_dentry);

release_cino:
	inode_unlock(child_inode);
	iput(child_inode);

release_pde:
	dput(parent_dentry);

release_pino:
	inode_unlock(dir_inode);
	iput(dir_inode);
	return (long) ret;
}

long evfs_dentry_read(struct super_block * sb, struct ext4_evfs_de_read_args * args)
{
	pr_info("ext4: READ_DENTRY called\n");
	struct inode * dir_inode;
	struct buffer_head * bh = NULL;
	struct ext4_dir_entry_2 * curr_dentry;
	unsigned int offset = 0;
	unsigned int blocksize;
	unsigned int entry_count = 0;
	int err = 0;

	// get directory inode
	dir_inode = ext4_iget(sb, args->in.dir_ino_num, EXT4_IGET_NORMAL);
	if (IS_ERR(dir_inode)) {
		err = PTR_ERR(dir_inode);
		goto out;
	}

	inode_lock_shared(dir_inode);

	// verify it is a directory
	if (!(S_ISDIR(dir_inode->i_mode))) {
		err = -ENOTDIR;
		goto dir_check_fail;
	}

	blocksize = dir_inode->i_sb->s_blocksize;

	// read first directory block
	bh = ext4_bread(NULL, dir_inode, 0, 0);
	if (IS_ERR_OR_NULL(bh)) {
		err = bh ? PTR_ERR(bh) : -EIO;
		goto dir_check_fail;
	}

	curr_dentry = (struct ext4_dir_entry_2 *) bh->b_data;

	// iterate thru dentries till we find the specified one
	unsigned int trailing_checksum_size = 0; // space reserved at the end of the block for the checksum
	if (ext4_has_metadata_csum(sb)) {	// check if this fs is using metadata checksums
		trailing_checksum_size = sizeof(struct ext4_dir_entry_tail);
	}

	while (offset < blocksize - trailing_checksum_size) {

		unsigned int rec_len = le16_to_cpu(curr_dentry->rec_len);

		if (rec_len == 0) {	// end of entries
			break;
		}

		if (curr_dentry->inode != 0) {	// skip deleted entries
			if (entry_count == args->in.target_dentry_index) {	// found specified dentry
				// copy info of this found dentry
				args->out.ino_num = le32_to_cpu(curr_dentry->inode);
				args->out.file_type = curr_dentry->file_type;
				args->out.name_len = curr_dentry->name_len;
				memcpy(args->out.name, curr_dentry->name, curr_dentry->name_len);
				args->out.name[EXT4_NAME_LEN - 1] = 0;
				goto release;
			}

			entry_count++;
		}

		offset += rec_len;
		curr_dentry = (struct ext4_dir_entry_2 *)((char *)curr_dentry + rec_len);

	}

	// didn't find specified inode
	args->out.ino_num = 0;
	args->out.file_type = 0;
	args->out.name_len = 0;
	args->out.name[0] = 0;

release:
	brelse(bh);
dir_check_fail:
	inode_unlock_shared(dir_inode);
	iput(dir_inode);
out:
	return (long) err;
}

long evfs_dentry_delete(struct super_block * sb, struct ext4_evfs_de_delete_args * args)
{
	// suggestion: use `lookup_one_len` to replace finding 
	// 	       inode manually to improve maintainability
	struct buffer_head * bh = NULL;
	struct inode * dir_inode;
	struct inode * child_inode = NULL;
	struct dentry * parent_dentry;
	struct dentry * target_dentry;
	struct qstr qname;
	struct ext4_dir_entry_2 * de;
	bool parent_dput_called = false;
	bool child_dput_called = false;
	int ret = 0, err;

	// get directory inode
	dir_inode = ext4_iget(sb, args->in.dir_ino_num, EXT4_IGET_NORMAL);
	if (IS_ERR(dir_inode)) {
		ret = PTR_ERR(dir_inode);
		goto out;
	}

	// lock the parent inode
	inode_lock(dir_inode);

	// verify it is a directory
	if (!(S_ISDIR(dir_inode->i_mode))) {
		ret = -ENOTDIR;
		goto release_pino;
	}

	// look up parent dentry
	parent_dentry = d_find_any_alias(dir_inode);
	if (!parent_dentry) {
		ret = -ENOENT;
		goto release_pino;
	}

	qname.name = args->in.name;
	qname.len = strlen(args->in.name);
	qname.hash = full_name_hash(parent_dentry, qname.name, qname.len);

	if (IS_CASEFOLDED(dir_inode)) {
		sb->s_d_op->d_hash(parent_dentry, &qname);
	}

	bh = ext4_find_entry(dir_inode, &qname, &de, NULL);
	if (IS_ERR_OR_NULL(bh)) {
		ret = bh ? PTR_ERR(bh) : -ENOENT;
		goto release_pde;
	}

	// get child inode
	uint32_t child_ino_num = le32_to_cpu(de->inode);
	brelse(bh);

	child_inode = ext4_iget(sb, child_ino_num, EXT4_IGET_NORMAL);
	if (IS_ERR(child_inode)) {
		ret = PTR_ERR(child_inode);
		goto release_pde;
	}

	inode_lock(child_inode);	
	if ((err = __evfs_inode_untrack_nolock(child_inode)) < 0) {
		ret = err;
		goto release_cino;
	}

	target_dentry = d_alloc(parent_dentry, &qname);
	if (!target_dentry) {
		ret = -ENOMEM;
		goto release_cino;
	}

	d_add(target_dentry, child_inode);

	// DONE: initialize disk quotas for dir and child to make
	// sure fs with disk quota enabled doesn't run out
	// of journal credits
	if ((err = dquot_initialize(dir_inode))) {
		ret = err;
		goto release_cde;
	}

	if ((err = dquot_initialize(child_inode))) {
		ret = err;
		goto release_cde;
	}

	if ((err = __ext4_unlink(dir_inode, &qname, child_inode, target_dentry))) {
		ret = err;
	}

release_cde:
	// cleanup
	dput(target_dentry);
	child_dput_called = true;

release_cino:
	inode_unlock(child_inode);
	if (!child_dput_called) iput(child_inode);

release_pde:
	dput(parent_dentry);
	parent_dput_called = true;

release_pino:
	if (!parent_dput_called) inode_unlock(dir_inode);
	iput(dir_inode);

out:
	return (long) ret;
}

long evfs_dentry_update(struct super_block * sb, struct ext4_evfs_de_update_args * args)
{
	struct inode * dir_inode;
	struct buffer_head * bh = NULL;
	struct ext4_dir_entry_2 * curr_dentry;
	handle_t * handle;
	unsigned int offset = 0;
	unsigned int blocksize;
	unsigned int entry_count = 0;
	unsigned long old_ino_num = -1;
	struct inode * new_inode;
	struct inode * old_inode;
	bool ocino_iget_called = false;
	bool ncino_iget_called = false;
	int ret = 0, err;

	// get directory inode
	dir_inode = ext4_iget(sb, args->in.dir_ino_num, EXT4_IGET_NORMAL);
	if (IS_ERR(dir_inode)) {
		ret = PTR_ERR(dir_inode);
		return ret;
	}

	inode_lock(dir_inode);
	// verify it is a directory
	if (!(S_ISDIR(dir_inode->i_mode))) {
		ret = -ENOTDIR;
		goto release_pino;
	}

	blocksize = dir_inode->i_sb->s_blocksize;
	// start journal transaction
	handle = ext4_journal_start(dir_inode, EXT4_HT_DIR, 4);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto release_pino;
	}

	// read first directory block
	bh = ext4_bread(NULL, dir_inode, 0, 0);
	if (IS_ERR_OR_NULL(bh)) {
		ret = bh ? PTR_ERR(bh) : -EIO;
		goto stop_journal;
	}

	// get write access to directory block
	if ((err = ext4_journal_get_write_access(handle, sb, bh, EXT4_JTR_NONE))) {
		ret = err;
		goto release_bh;
	}

	curr_dentry = (struct ext4_dir_entry_2 *) bh->b_data;

	// iterate through to find the ith dentry
	unsigned int trailing_checksum_size = 0; // space reserved at the end of the block for the checksum
	if (ext4_has_metadata_csum(sb)) {	// check if this fs is using metadata checksums
		trailing_checksum_size = sizeof(struct ext4_dir_entry_tail);
	}

	while (offset < blocksize - trailing_checksum_size) {
		unsigned int rec_len = le16_to_cpu(curr_dentry->rec_len);
		if (rec_len == 0) break; // end of entries
		if (curr_dentry->inode != 0) {	// skip deleted entries
			// found target dentry
			if (entry_count == args->in.target_dentry_index) {
				old_ino_num = curr_dentry->inode;
				break;
			}

			entry_count++;
		}

		offset += rec_len;
		curr_dentry = (struct ext4_dir_entry_2 *)((char *) curr_dentry + rec_len);
	}

	if (old_ino_num >= 0) {
		new_inode = ext4_iget(sb, args->in.new_ino_num, EXT4_IGET_NORMAL);
		if (IS_ERR(new_inode)) {
			ret = PTR_ERR(new_inode);
			goto release_bh;
		}

		ncino_iget_called = true;
		inode_lock(new_inode);
		if ((err = __evfs_inode_inc_nlink(handle, new_inode)) < 0) {
			ret = err;
			goto release_bh;
		}

		inode_unlock(new_inode);

		old_inode = ext4_iget(sb, old_ino_num, EXT4_IGET_NORMAL);
		if (IS_ERR(old_inode)) {
			ret = PTR_ERR(old_inode);
			goto release_bh;
		}

		ocino_iget_called = true;
		inode_lock(old_inode);
		if ((err =__evfs_inode_dec_nlink(handle, old_inode)) < 0) {
			ret = err;
			goto release_bh;	
		}

		if (old_inode->i_nlink == 0) {
			if ((err = __evfs_inode_untrack_nolock(old_inode)) < 0) {
				ret = err;
				goto release_bh;
			}

			ext4_orphan_add(handle, old_inode);
		}

		inode_unlock(old_inode);
		curr_dentry->inode = cpu_to_le32(args->in.new_ino_num);	
		if ((err = ext4_handle_dirty_dirblock(handle, dir_inode, bh))) {
			ret = err;
			goto release_bh;
		}

		// DONE: modify the directory inode ctime & mtime
		struct timespec64 ts = inode_set_ctime_current(dir_inode);
		inode_set_mtime_to_ts(dir_inode, ts);

		if ((err = ext4_mark_inode_dirty(handle, dir_inode))) {
			ret = err;
			goto release_bh;
		}
	} else {
		ret = -EINVAL;
	}

release_bh:
	brelse(bh);

stop_journal:
	if ((err = ext4_journal_stop(handle)) < 0) {
		ret = err;
	}

	if (ncino_iget_called) iput(new_inode);
	if (ocino_iget_called) iput(old_inode);

release_pino:
	inode_unlock(dir_inode);
	iput(dir_inode);
	return (long) err;
}

// TODO: add support to read out extents from the index-based inode
long evfs_extent_read(struct super_block * sb, struct ext4_evfs_ext_read_args * args)
{
	struct ext4_ext_path *path = NULL;
	__u32 count = 0;
	ext4_lblk_t block = 0;
	bool is_ext_tree_locked = false;
	int err = 0;

	// get target inode
	struct inode * inode = ext4_iget(sb, args->in.ino_num, EXT4_IGET_NORMAL);
	if (IS_ERR(inode)) {
		return PTR_ERR(inode);
	}

	/* Lock extent tree */
	down_read(&EXT4_I(inode)->i_data_sem);
	is_ext_tree_locked = true;

	while (count < args->in.max_num_exts) {
		// find which extent covers <block>, or the next that follows it	
		// each inode uses an extent tree. Extent is stored at leaf. Path[-1] is leaf
		path = ext4_find_extent(inode, block, &path, 0);
		if (IS_ERR(path)) {
			path = NULL;
			break;
		}

		/* Find depth of extent tree */
		int depth = ext_depth(inode);
		/* path[depth] is leaf of extree */
		struct ext4_extent *ex = path[depth].p_ext;	/* p_ext points to extent */
		if (!ex) {
			break;	/* no extents */
		}
		/* Check if we are looking at the same extent over and over, it means there's no more */
		if (le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex) <= block) {
			break;
		}

		// construct our custom extent struct
		args->out.exts[count].log_start = le32_to_cpu(ex->ee_block);
		args->out.exts[count].phy_start = ext4_ext_pblock(ex);
		args->out.exts[count].len = ext4_ext_get_actual_len(ex);
		count++;

		block = args->out.exts[count].log_start + args->out.exts[count].len;
	}


	if (is_ext_tree_locked) up_read(&EXT4_I(inode)->i_data_sem);

	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}

	if (inode) iput(inode);

	args->out.num_exts = count;
	return err;
}

/**
 * @brief extent move
 *
 * ASSUMPTION:
 * ----: Before: Allocate blocks that no-one uses
 * ----: After: Free or map those blocks to existing inode
 *
 * NOTE: see fs/ext4/extents.c:ext4_move_extent for how data is copied
 * NOTE: see fs/ext4/extents.c:ext4_swap_extent for how extent is swapped
 * NOTE: since we're 'replacing', we do pure swap for now
 * DONE: enable outputing the old extents so user space know which blocks
 * ----: to release
 * TODO: add support for indexed based inode
 *
 */
long evfs_extent_move(struct super_block * sb, struct ext4_evfs_ext_mv_args * args)
{
	struct inode * inode;
	handle_t *handle = NULL;
	int ret = 0, err;

	/* User array of new extents */
	inode = ext4_iget(sb, args->in.ino_num, EXT4_IGET_NORMAL); 
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto out;
	}
	inode_lock(inode);
	down_write(&EXT4_I(inode)->i_data_sem);

	unsigned long long iver;
	evfs_iver(inode, &iver);
	if (iver != args->in.exp_iver) {
		ret = -EAGAIN;
		goto release_inode;
	}

	int credits = ext4_writepage_trans_blocks(inode);
	handle = ext4_journal_start(inode, EXT4_HT_MOVE_EXTENTS, credits);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto release_inode;
	}

	// DONE: Wait for all existing direct I/O workers
	inode_dio_wait(inode);

	struct ext4_evfs_ext next, curr;
	memcpy(&curr, &(args->in.ext), sizeof(struct ext4_evfs_ext)); //create a copy
	args->out.num_exts = 0;
	while (curr.len != 0 && args->out.num_exts < args->in.ext.len) {
		if ((err = __evfs_extent_map(handle, inode, &curr, &next)) < 0) {
			ret = err;
			break;
		}

		memcpy(&(args->out.exts[args->out.num_exts++]), &curr, sizeof(struct ext4_evfs_ext));
		memcpy(&curr, &next, sizeof(struct ext4_evfs_ext));
	}
	
	if ((err = ext4_journal_stop(handle))) {
		ret = err;
	}

release_inode:
	up_write(&EXT4_I(inode)->i_data_sem);
	inode_unlock(inode);
	iput(inode);

out:
	return (long) ret;
}

long ext4_ioctl_evfs(unsigned int cmd, struct inode * ino, struct super_block * sb, unsigned long arg)
{
	long ret;
	switch(cmd) {
		case EXT4_EVFS_HELLO:
			return evfs_hello();
		case EXT4_EVFS_IVER:
			unsigned long long u_ver;
			ret = evfs_iver(ino, &u_ver);
			if (copy_to_user((void __user *) arg, &u_ver, sizeof(u_ver)))
				return -EFAULT;

			return ret;
		case EXT4_EVFS_BLK_ALLOC:
			return evfs_block_alloc(ino, sb, (ext4_fsblk_t) arg);
		case EXT4_EVFS_BLK_FREE:
			return evfs_block_free(ino, sb, (ext4_fsblk_t) arg);
		case EXT4_EVFS_INO_ALLOC:
			return evfs_inode_alloc(sb, (ino_t) arg);
		case EXT4_EVFS_INO_FREE:
			return evfs_inode_free(sb, (ino_t) arg);
		case EXT4_EVFS_INO_READ:
		{
			struct evfs_ino_read_args k_args;
			struct evfs_ino_read_args __user * u_args =
				(struct evfs_ino_read_args __user *) arg;

			if (copy_from_user(&(k_args.in), &(u_args->in),
						sizeof(k_args.in)))
				return -EFAULT;
			
			ret = evfs_inode_read(sb, &k_args);
			if (copy_to_user(&(u_args->out), &(k_args.out),
						sizeof(k_args.out)))
				return -EFAULT;

			return ret;
		}
		case EXT4_EVFS_DEN_ADD:
		{
			struct ext4_evfs_de_add_args k_args;
			struct ext4_evfs_de_add_args __user * u_args =
				(struct ext4_evfs_de_add_args __user *) arg;

			if (copy_from_user(&(k_args.in), &(u_args->in), sizeof(k_args.in)))
				return -EFAULT;

			k_args.in.name[sizeof(k_args.in.name) - 1] = 0;
			return evfs_dentry_add(sb, &k_args);
		}
		case EXT4_EVFS_DEN_READ:
		{
			struct ext4_evfs_de_read_args k_args;
			struct ext4_evfs_de_read_args __user * u_args =
				(struct ext4_evfs_de_read_args __user *) arg;

			if (copy_from_user(&(k_args.in), &(u_args->in),
						sizeof(k_args.in)))
				return -EFAULT;
			
			ret = evfs_dentry_read(sb, &k_args);
			if (copy_to_user(&(u_args->out), &(k_args.out),
						sizeof(k_args.out)))
				return -EFAULT;

			return ret;
		}
		case EXT4_EVFS_DEN_DELETE:
		{
			struct ext4_evfs_de_delete_args k_args;
			struct ext4_evfs_de_delete_args __user * u_args =
				(struct ext4_evfs_de_delete_args __user *) arg;
			if (copy_from_user(&(k_args.in), &(u_args->in), sizeof(k_args.in)))
				return -EFAULT;

			k_args.in.name[sizeof(k_args.in.name) - 1] = 0;
			return evfs_dentry_delete(sb, &k_args);
		}
		case EXT4_EVFS_DEN_UPDATE:
		{
			struct ext4_evfs_de_update_args k_args;
			struct ext4_evfs_de_update_args __user * u_args =
				(struct ext4_evfs_de_update_args __user *) arg;
			if (copy_from_user(&(k_args.in), &(u_args->in), sizeof(k_args.in)))
				return -EFAULT;

			ret = evfs_dentry_update(sb, &k_args);
			return ret;
		}
		case EXT4_EVFS_EXT_READ:
		{
			struct ext4_evfs_ext_read_args k_args;
			struct ext4_evfs_ext_read_args __user * u_args =
				(struct ext4_evfs_ext_read_args __user *) arg;
			void __user * u_exts;

			if (copy_from_user(&(k_args.in), &(u_args->in), sizeof(k_args.in)))
				return -EFAULT;
			
			if (copy_from_user(&u_exts, &(u_args->out.exts), sizeof(void *)))
				return -EFAULT;
			
			size_t ext_arr_size = sizeof(struct ext4_evfs_ext) * k_args.in.max_num_exts;
			k_args.out.exts = (struct ext4_evfs_ext *) kmalloc(ext_arr_size, GFP_KERNEL);

			ret = evfs_extent_read(sb, &k_args);
			if (copy_to_user(&(u_args->out.num_exts), &(k_args.out.num_exts), sizeof(unsigned int)))
				return -EFAULT;

			if (copy_to_user(u_exts, k_args.out.exts, ext_arr_size))
				return -EFAULT;

			kfree(k_args.out.exts);
			return ret;
		}
		case EXT4_EVFS_EXT_MV:
		{
			struct ext4_evfs_ext_mv_args k_args;
			struct ext4_evfs_ext_mv_args __user * u_args =
				(struct ext4_evfs_ext_mv_args __user *) arg;
			void __user * u_exts;

			if (copy_from_user(&(k_args.in), &(u_args->in), sizeof(k_args.in)))
				return -EFAULT;

			if (copy_from_user(&u_exts, &(u_args->out.exts), sizeof(void *)))
				return -EFAULT;
			
			size_t ext_arr_size = sizeof(struct ext4_evfs_ext) * k_args.in.ext.len;
			k_args.out.exts = (struct ext4_evfs_ext *) kmalloc(ext_arr_size, GFP_KERNEL);
			
			ret = evfs_extent_move(sb, &k_args);
			if (copy_to_user(&(u_args->out.num_exts), &(k_args.out.num_exts), sizeof(unsigned int)))
				return -EFAULT;			

			if (copy_to_user(u_exts, k_args.out.exts, ext_arr_size))
				return -EFAULT;

			kfree(k_args.out.exts);
			return ret;
		}
		default:
			return -ENOTTY;
	}
}

