#include <linux/fs.h>
#include <linux/capability.h>
#include <linux/time.h>
#include <linux/compat.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <linux/quotaops.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/iversion.h>
#include <linux/fileattr.h>
#include <linux/uuid.h>
#include "ext4_jbd2.h"
#include "ext4.h"
#include <linux/fsmap.h>
#include "fsmap.h"
#include <trace/events/ext4.h>
#include "ext4-evfs.h"

long __ext4_evfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct inode *inode = file_inode(filp);
	struct super_block *sb = inode->i_sb;

    switch(cmd) {
        case EXT4_IOC32_PRINTHELLO: 
		pr_info("ext4: HELLO\n");
		return 0;
	case EXT4_IOC_FLIP_BLOCK_BIT: {
		// TODO: update free block counter
		__u64 block_number;
		ext4_group_t group;
		ext4_grpblk_t offset;
		struct buffer_head *bitmap_bh;
		struct ext4_group_desc *group_descriptor;
		struct buffer_head *group_descriptor_bh;	// buffer for group descriptor block
        struct ext4_sb_info *sbi = EXT4_SB(sb); // superblock info
		handle_t *journal_handle;	// one active transaction in the journal
		int err;
        int was_set;    // whether bit was set BEFORE the flip

		if (copy_from_user(&block_number, (void __user *)arg, sizeof(block_number))) {
			return -EFAULT;
		}

		ext4_get_group_no_and_offset(sb, (ext4_fsblk_t)block_number, &group, &offset);

		bitmap_bh = ext4_read_block_bitmap(sb, group);
		if (IS_ERR(bitmap_bh)) {
			return PTR_ERR(bitmap_bh);
		}

		group_descriptor = ext4_get_group_desc(sb, group, &group_descriptor_bh);
		if (!group_descriptor) {
			brelse(bitmap_bh);
			return -EIO;
		}

		// start a journal transaction
		// 3 blocks are affected (group descriptor, data bitmap, superblock)
		journal_handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 3);
		if (IS_ERR(journal_handle)) {
			brelse(bitmap_bh);
			return PTR_ERR(journal_handle);
		}

		// get write access to bitmap thru journal
		err = ext4_journal_get_write_access(journal_handle, sb, bitmap_bh, EXT4_JTR_NONE);
		if (err) {
			goto out_journal;
		}

		// get write access to group descriptor thru journal
		err = ext4_journal_get_write_access(journal_handle, sb, group_descriptor_bh, EXT4_JTR_NONE);
		if (err) {
			goto out_journal;
		}

        /*
        write access is not needed for superblock. percpu() are atomic in-mem updates
        */

		// flip bit
		was_set = ext4_test_bit(offset, bitmap_bh->b_data);
		if (was_set) {
			ext4_clear_bit(offset, bitmap_bh->b_data);
			pr_info("ext4: Cleared bit %d in group %u\n", offset, group);
		} else {
			ext4_set_bit(offset, bitmap_bh->b_data);
			pr_info("ext4: Set bit %d in group %u\n", offset, group);
		}

        // update group descriptor free block count
        if (was_set) {  // was 1, now is 0. free blocks is 1 more
            ext4_free_group_clusters_set(sb, group_descriptor, 
                ext4_free_group_clusters(sb, group_descriptor) + 1);
        } else {        // was 0 and now 1, one less free block
            ext4_free_group_clusters_set(sb, group_descriptor, 
                ext4_free_group_clusters(sb, group_descriptor) - 1);
        }
        // update superblock free block count
        if (was_set) {
            percpu_counter_add(&sbi->s_freeclusters_counter, 1);
        } else {
            percpu_counter_sub(&sbi->s_freeclusters_counter, 1);
        }

		// update bitmap checksum
		ext4_block_bitmap_csum_set(sb, group_descriptor, bitmap_bh);
		// update group descriptor checksum
		ext4_group_desc_csum_set(sb, group, group_descriptor);
        /*
        superblock checksum is automatically updated when 
        the superblock is written to disk
        */

		// add bitmap changes to transaction
		err = ext4_handle_dirty_metadata(journal_handle, NULL, bitmap_bh);
		if (err) { goto out_journal; }

		// add group descriptor changes to transaction 
		err = ext4_handle_dirty_metadata(journal_handle, NULL, group_descriptor_bh);
		if (err) { goto out_journal; }

		// commit transaction
		// keep this separate from out_journal since we are updating err here
		err = ext4_journal_stop(journal_handle);
		brelse(bitmap_bh);
		return err;

out_journal:
		ext4_journal_stop(journal_handle);
		brelse(bitmap_bh);
		return err;
}
    default:
        return -ENOTTY;
    }
}