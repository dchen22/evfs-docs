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
	case EXT4_IOC_ADD_DENTRY: {
		struct ext4_evfs_dentry_add added_dentry_info;
		struct inode * dir_inode;
		struct buffer_head * bh = NULL;
		struct ext4_dir_entry_2 * curr_dentry;
		handle_t * handle;
		int err;
		unsigned int blocksize;
		unsigned int offset;
		char * kaddr;

		// copy from userspace
		if (copy_from_user(&added_dentry_info, (void __user *)arg, sizeof(added_dentry_info))) {
			return -EFAULT;
		}

		// validate name length
		added_dentry_info.name[sizeof(added_dentry_info.name) - 1] = '\0'; // null terminate
		size_t name_len = strlen(added_dentry_info.name);
		if (name_len == 0 || name_len > EXT4_NAME_LEN) {
			pr_warn("ext4-evfs: invalid name length %zu\n", name_len);
			return -EINVAL;
		}

		// get parent directory inode
		dir_inode = ext4_iget(sb, added_dentry_info.parent_inode_number, EXT4_IGET_NORMAL);
		if (IS_ERR(dir_inode)) {
			err = PTR_ERR(dir_inode);
			pr_warn("ext4-evfs: failed to get directory inode %llu: %d\n",
					added_dentry_info.parent_inode_number, err);
			return err;
		}

		// verify it is a directory
		if (!(S_ISDIR(dir_inode->i_mode))) {
			pr_warn("ext4-evfs: parent inode %llu is not a directory\n",
					added_dentry_info.parent_inode_number);
			iput(dir_inode);
			return -ENOTDIR;
		}

		blocksize = dir_inode->i_sb->s_blocksize;

		// start journal transaction
		// only modify dentry block for now
		handle = ext4_journal_start(dir_inode, EXT4_HT_DIR, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			iput(dir_inode);
			return err;
		}

		// read the first block of the directory
		bh = ext4_bread(handle, dir_inode, 0, 0);
		if (IS_ERR(bh)) {
			err = PTR_ERR(bh);
			pr_warn("ext4-evfs: failed to read directory block: %d\n", err);
			goto out_stop;
		}
		if (!bh) {
			pr_warn("ext4-evfs: directory block is NULL\n");
			err = -EIO;
			goto out_stop;
		}

		// get write access to directory entry block
		err = ext4_journal_get_write_access(handle, sb, bh, EXT4_JTR_NONE);
		if (err) {
			pr_warn("ext4-evfs: failed to get write access: %d\n", err);
			goto out_brelse;
		}

		kaddr = (char *)bh->b_data;

		// find space at the end of the dentry block
		offset = 0;
		curr_dentry = (struct ext4_dir_entry_2 *)kaddr;	// current dentry as we iterate thru directory block

		// find the end of the existing entries
		while (offset < blocksize) {
			if (curr_dentry->rec_len == 0) {	
				// invalid entry
				break;
			}

			// check if this is the last entry
			unsigned int actual_len = EXT4_DIR_REC_LEN(curr_dentry->name_len);
			unsigned int rec_len = le16_to_cpu(curr_dentry->rec_len);

			// if there is a difference, it means there is slack (i.e. we're pointing to the last entry)
			if (rec_len >= actual_len + EXT4_DIR_REC_LEN(name_len)) {
				// there is enough space after this entry
				struct ext4_dir_entry_2 * new_dentry;
				
				// shrink current entry
				curr_dentry->rec_len = cpu_to_le16(actual_len);

				// create new dentry after it
				new_dentry = (struct ext4_dir_entry_2 *)((char *)curr_dentry + actual_len);
				new_dentry->inode = cpu_to_le32(added_dentry_info.child_inode_number);
				new_dentry->rec_len = cpu_to_le16(rec_len - actual_len);	// give new dentry to-the-end length allocation
				new_dentry->name_len = name_len;
				new_dentry->file_type = added_dentry_info.file_type;
				memcpy(new_dentry->name, added_dentry_info.name, name_len);

				pr_info("ext4-evfs: added raw dentry '%s' (ino=%llu, type=%u) to dir %llu\n",
						added_dentry_info.name, added_dentry_info.child_inode_number,
						added_dentry_info.file_type, added_dentry_info.parent_inode_number);

				// mark buffer dirty
				err = ext4_handle_dirty_dirblock(handle, dir_inode, bh);
				goto out_brelse;
			}

			offset += rec_len;
			curr_dentry = (struct ext4_dir_entry_2 *)(kaddr + offset);
		}

		pr_warn("ext4-evfs: no space in directory block\n");
		err = -ENOSPC;
out_brelse:
		brelse(bh);
out_stop:
		ext4_journal_stop(handle);
		iput(dir_inode);
		return err;
}
	case EXT4_IOC_READ_DENTRY: {
		struct ext4_evfs_dentry_read read_info;
		struct inode * dir_inode;
		struct buffer_head * bh = NULL;
		struct ext4_dir_entry_2 * curr_dentry;
		unsigned int offset = 0;
		unsigned int blocksize;
		char * output;
		int output_len = 0;
		int err;

		// copy from userspace
		if (copy_from_user(&read_info, (void __user *)arg, sizeof(read_info))) {
			return -EFAULT;
		}

		// get directory inode
		dir_inode = ext4_iget(sb, read_info.dir_inode_number, EXT4_IGET_NORMAL);
		if (IS_ERR(dir_inode)) {
			err = PTR_ERR(dir_inode);
			pr_warn("ext4-evfs: failed to get directory inode: %llu: %d\n",
					read_info.dir_inode_number, err);
			return err;
		}

		// verify it is a directory
		if (!(S_ISDIR(dir_inode->i_mode))) {
			pr_warn("ext4-evfs: inode %llu is not a directory\n", read_info.dir_inode_number);
			iput(dir_inode);
			return -ENOTDIR;
		}

		blocksize = dir_inode->i_sb->s_blocksize;
		output = read_info.buffer;

		// read first directory block
		bh = ext4_bread(NULL, dir_inode, 0, 0);
		if (IS_ERR_OR_NULL(bh)) {
			err = bh ? PTR_ERR(bh) : -EIO;
			pr_warn("ext4-evfs: failed to read directory block:%d\n", err);
			iput(dir_inode);
			return err;
		}

		curr_dentry = (struct ext4_dir_entry_2 *)bh->b_data;

		// iterate thru dentries
		while (offset < blocksize && output_len < sizeof(read_info.buffer) - 256) {
			unsigned int rec_len = le16_to_cpu(curr_dentry->rec_len);

			if (rec_len == 0) {	// corruption
				break;
			}

			if (curr_dentry->inode != 0) {	// skip deleted entries
				int written = snprintf(
					output + output_len,
					sizeof(read_info.buffer) - output_len,
					"inode=%u, type=%u, len=%u, name='%.*s'\n",
					le32_to_cpu(curr_dentry->inode),
					curr_dentry->file_type,
					curr_dentry->name_len,
					curr_dentry->name_len,
					curr_dentry->name
					);

				if (written > 0) {
					output_len += written;
				}
			}

			offset += rec_len;
			curr_dentry = (struct ext4_dir_entry_2 *)((char *)curr_dentry + rec_len);

		}

		brelse(bh);
		iput(dir_inode);

		// copy result back to userspace
		if (copy_to_user((void __user *)arg, &read_info, sizeof(read_info))) {
			return -EFAULT;
		}

		return 0;
	}
    default:
        return -ENOTTY;
    }
}