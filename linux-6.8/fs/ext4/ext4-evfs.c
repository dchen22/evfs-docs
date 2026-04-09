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
#include "../internal.h"

int ext4_add_entry(handle_t *handle, struct dentry *dentry,
			  struct inode *inode);
int ext4_delete_entry(handle_t *handle,
			     struct inode *dir,
			     struct ext4_dir_entry_2 *de_del,
			     struct buffer_head *bh);
struct buffer_head *ext4_find_entry(struct inode *dir,
					   const struct qstr *d_name,
					   struct ext4_dir_entry_2 **res_dir,
					   int *inlined);
struct buffer_head *
ext4_read_inode_bitmap(struct super_block *sb, ext4_group_t block_group);

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
		ext4_lock_group(sb, group);
		was_set = ext4_test_bit(offset, bitmap_bh->b_data);
		if (was_set) {
			ext4_clear_bit(offset, bitmap_bh->b_data);
			pr_info("ext4: Cleared bit %d in group %u\n", offset, group);
		} else {
			ext4_set_bit(offset, bitmap_bh->b_data);
			pr_info("ext4: Set bit %d in group %u\n", offset, group);
		}
		ext4_unlock_group(sb, group);

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
		pr_info("ext4: ADD_DENTRY called\n");
		struct ext4_evfs_add_dentry add_info;
		struct inode * dir_inode;
		struct inode * child_inode;
		struct dentry * parent_dentry;
		struct dentry * new_dentry;
		struct qstr qname;
		int err;

		// copy from userspace
		if (copy_from_user(&add_info, (void __user *)arg, sizeof(add_info))) {
			return -EFAULT;
		}

		add_info.name[sizeof(add_info.name) - 1] = '\0';
		size_t name_len = strlen(add_info.name);
		if (name_len == 0 || name_len > EXT4_NAME_LEN) {
			pr_warn("ext4-evfs: invalid name length %zu\n", name_len);
			return -EINVAL;
		}

		// get parent directory inode
		dir_inode = ext4_iget(sb, add_info.parent_inode_number, EXT4_IGET_NORMAL);
		if (IS_ERR(dir_inode)) {
			return PTR_ERR(dir_inode);
		}
		// ensure parent is a dir
		if (!S_ISDIR(dir_inode->i_mode)) {
			pr_warn("ext4-evfs: parent inode %llu is not a directory\n", add_info.parent_inode_number);
			iput(dir_inode);
			return -ENOTDIR;
		}

		// get child inode (this must exist)
		child_inode = ext4_iget(sb, add_info.child_inode_number, EXT4_IGET_NORMAL);
		if (IS_ERR(child_inode)) {
			iput(dir_inode);	// todo: what is this
			return PTR_ERR(child_inode);
		}

		// create a dentry for the parent
		parent_dentry = d_find_any_alias(dir_inode);
		if (!parent_dentry) {
			parent_dentry = d_alloc_pseudo(sb, &(struct qstr)QSTR_INIT("/", 1));
			if (!parent_dentry) {
				iput(child_inode);
				iput(dir_inode);
				return -ENOMEM;
			}
			d_instantiate(parent_dentry, dir_inode);
		}

		// create dentry for new entry
		qname.name = add_info.name;
		qname.len = strlen(add_info.name);
		qname.hash = 0;

		new_dentry = d_alloc(parent_dentry, &qname);
		if (!new_dentry) {
			dput(parent_dentry);
			iput(child_inode);
			iput(dir_inode);
			return -ENOMEM;
		}

		err = __ext4_link(dir_inode, child_inode, new_dentry);

		// cleanup
		dput(new_dentry);
		dput(parent_dentry);
		iput(child_inode);
		iput(dir_inode);

		if (err) {
			pr_warn("ext4-evfs: __ext4_link failed: %d\n", err);
			return err;
		}

		pr_info("ext4-evfs: added entry '%s' (parent=%llu, child=%llu)\n",
				add_info.name, add_info.parent_inode_number, add_info.child_inode_number);

		return 0;
	}

	case EXT4_IOC_READ_DENTRY: {
		pr_info("ext4: READ_DENTRY called\n");
		struct ext4_evfs_read_dentry read_info;
		struct inode * dir_inode;
		struct buffer_head * bh = NULL;
		struct ext4_dir_entry_2 * curr_dentry;
		unsigned int offset = 0;
		unsigned int blocksize;
		unsigned int entry_count = 0;
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

		// read first directory block
		bh = ext4_bread(NULL, dir_inode, 0, 0);
		if (IS_ERR_OR_NULL(bh)) {
			err = bh ? PTR_ERR(bh) : -EIO;
			pr_warn("ext4-evfs: failed to read directory block:%d\n", err);
			iput(dir_inode);
			return err;
		}

		curr_dentry = (struct ext4_dir_entry_2 *)bh->b_data;

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
				if (entry_count == read_info.target_dentry_index) {	// found specified dentry
					// copy info of this found dentry
					read_info.inode_number = le32_to_cpu(curr_dentry->inode);
					read_info.file_type = curr_dentry->file_type;
					read_info.name_len = curr_dentry->name_len;
					memcpy(read_info.name, curr_dentry->name, curr_dentry->name_len);
					read_info.name[curr_dentry->name_len] = '\0';

					brelse(bh);
					iput(dir_inode);

					// copy back to userspace
					if (copy_to_user((void __user *)arg, &read_info, sizeof(read_info))) {
						return -EFAULT;
					}

					return 0;
				}

				entry_count++;
			}

			offset += rec_len;
			curr_dentry = (struct ext4_dir_entry_2 *)((char *)curr_dentry + rec_len);

		}

		// didn't find specified inode
		read_info.inode_number = 0;
		read_info.file_type = 0;
		read_info.name_len = 0;
		read_info.name[0] = '\0';

		brelse(bh);
		iput(dir_inode);

		// copy result back to userspace
		if (copy_to_user((void __user *)arg, &read_info, sizeof(read_info))) {
			return -EFAULT;
		}

		return 0;
	}
	case EXT4_IOC_DELETE_DENTRY: {
		pr_info("ext4: DELETE_DENTRY called\n");

		struct ext4_evfs_delete_dentry delete_info;
		struct buffer_head * bh = NULL;
		struct inode * dir_inode;
		struct inode * child_inode = NULL;
		struct dentry * parent_dentry;
		struct dentry * delete_dentry;
		struct qstr qname;
		struct ext4_dir_entry_2 * de;	
		int err = 0;

		// copy from userspace
		if (copy_from_user(&delete_info, (void __user *)arg, sizeof(delete_info))) {
			return -EFAULT;
		}

		delete_info.name[sizeof(delete_info.name) - 1] = '\0';	// input sanitation

		// get directory inode
		dir_inode = ext4_iget(sb, delete_info.dir_inode_number, EXT4_IGET_NORMAL);
		if (IS_ERR(dir_inode)) {
			err = PTR_ERR(dir_inode);
			pr_warn("ext4-evfs: failed to get directory inode: %llu: %d\n",
					delete_info.dir_inode_number, err);
			return err;
		}

		// verify it is a directory
		if (!(S_ISDIR(dir_inode->i_mode))) {
			pr_warn("ext4-evfs: inode %llu is not a directory\n", delete_info.dir_inode_number);
			iput(dir_inode);
			return -ENOTDIR;
		}

		qname.name = delete_info.name;
		qname.len = strlen(delete_info.name);
		qname.hash = 0;

		bh = ext4_find_entry(dir_inode, &qname, &de, NULL);
		if (IS_ERR_OR_NULL(bh)) {
			pr_warn("ext4-evfs: entry '%s' not found\n", delete_info.name);
			iput(dir_inode);
			return bh ? PTR_ERR(bh) : -ENOENT;
		}

		// get child inode
		uint32_t child_inode_number = le32_to_cpu(de->inode);
		child_inode = ext4_iget(sb, child_inode_number, EXT4_IGET_NORMAL);
		brelse(bh);

		if (IS_ERR(child_inode)) {
			err = PTR_ERR(child_inode);
			iput(dir_inode);
			pr_warn("ext4-evfs: failed to get child inode: %d\n", err);
			return err;
		}

		// create dentries
		parent_dentry = d_find_any_alias(dir_inode);
		if (!parent_dentry) {
			parent_dentry = d_alloc_pseudo(sb, &(struct qstr)QSTR_INIT("/", 1));
			if (!parent_dentry) {
				iput(child_inode);
				iput(dir_inode);
				return -ENOMEM;
			}
			d_instantiate(parent_dentry, dir_inode);
		}

		delete_dentry = d_alloc(parent_dentry, &qname);
		if (!delete_dentry) {
			dput(parent_dentry);
			iput(child_inode);
			iput(dir_inode);
			return -ENOMEM;
		}

		err = __ext4_unlink(dir_inode, &qname, child_inode, delete_dentry);
	
		// cleanup
		dput(delete_dentry);
		dput(parent_dentry);
		iput(child_inode);
		iput(dir_inode);

		if (err) {
			pr_warn("ext4-evfs: __ext4_unlink failed: %d\n", err);
		} else {
			pr_info("ext4-evfs: deleted entry '%s' (inode number=%u) from parent=%llu\n",
					delete_info.name, child_inode_number, delete_info.dir_inode_number);
		}

		return err;


	}
	case EXT4_IOC_UPDATE_DENTRY: {
		pr_info("ext4: UPDATE_DENTRY called\n");

		struct ext4_evfs_update_dentry update_info;
		struct inode * dir_inode;
		struct buffer_head * bh = NULL;
		struct ext4_dir_entry_2 * curr_dentry;
		struct ext4_dir_entry_2 * target_dentry = NULL;
		handle_t * handle;
		unsigned int offset = 0;
		unsigned int blocksize;
		unsigned int entry_count = 0;
		int err = -ENOENT;

		// copy from userspace
		if (copy_from_user(&update_info, (void __user *)arg, sizeof(update_info))) {
			return -EFAULT;
		}

		// get directory inode
		dir_inode = ext4_iget(sb, update_info.dir_inode_number, EXT4_IGET_NORMAL);
		if (IS_ERR(dir_inode)) {
			err = PTR_ERR(dir_inode);
			pr_warn("ext4-evfs: failed to get directory inode: %llu: %d\n",
					update_info.dir_inode_number, err);
			return err;
		}

		// verify it is a directory
		if (!(S_ISDIR(dir_inode->i_mode))) {
			pr_warn("ext4-evfs: inode %llu is not a directory\n", update_info.dir_inode_number);
			iput(dir_inode);
			return -ENOTDIR;
		}

		blocksize = dir_inode->i_sb->s_blocksize;

		// start journal transaction
		handle = ext4_journal_start(dir_inode, EXT4_HT_DIR, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			iput(dir_inode);
			return err;
		}

		// read first directory block
		bh = ext4_bread(NULL, dir_inode, 0, 0);
		if (IS_ERR_OR_NULL(bh)) {
			err = bh ? PTR_ERR(bh) : -EIO;
			pr_warn("ext4-evfs: failed to read directory block:%d\n", err);
			goto update_dentry_out_stop;
		}

		// get write access to directory block
		err = ext4_journal_get_write_access(handle, sb, bh, EXT4_JTR_NONE);
		if (err) {
			pr_warn("ext4-evfs: failed to get write access to journal: %d\n", err);
			goto update_dentry_out_brelse;
		}

		curr_dentry = (struct ext4_dir_entry_2 *)bh->b_data;

		// iterate through to find the ith dentry
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
				// found target dentry
				if (entry_count == update_info.target_dentry_index) {	
					curr_dentry->inode = cpu_to_le32(update_info.new_inode_number);

					// mark buffer dirty (handles dirblock checksum)
					err = ext4_handle_dirty_dirblock(handle, dir_inode, bh);

					pr_info("ext4-evfs: updated entry %u to inode %u\n",
							entry_count, curr_dentry->inode);

					goto update_dentry_out_brelse;
				}

				entry_count++;
			}

			offset += rec_len;
			curr_dentry = (struct ext4_dir_entry_2 *)((char *)curr_dentry + rec_len);

		}

		if (err == -ENOENT) {
			pr_warn("ext4-evfs: update failed, index %u out of bounds\n",
					update_info.target_dentry_index);
		}

update_dentry_out_brelse:
		brelse(bh);
update_dentry_out_stop:
		ext4_journal_stop(handle);
		iput(dir_inode);
		return err;
}
	case EXT4_IOC_ITER_INODE: {
		struct ext4_evfs_iter_inode iter_info;
		if (copy_from_user(&iter_info, (void __user *)arg, sizeof(iter_info))) {
			return -EFAULT;
		}

		__u32 found = 0;
		__u32 total = le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count);

		for (__u32 i = iter_info.start_inode_number + 1; i <= total; i++) {
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

		iter_info.result_inode_number = found;
		if (copy_to_user((void __user *)arg, &iter_info, sizeof(iter_info))) {
			return -EFAULT;
		}
		return 0;
	}
	case EXT4_IOC_ITER_FREESPACE: {
		struct ext4_evfs_iter_freespace iter_info;
		if (copy_from_user(&iter_info, (void __user *)arg, sizeof(iter_info))) {
			return -EFAULT;
		}

		__u64 found_start = 0;
		__u64 found_length = 0;
		__u64 total_blocks = ext4_blocks_count(EXT4_SB(sb)->s_es);
		int in_free_extent = 0;

		for (__u64 b = iter_info.start_block + 1; b <= total_blocks; b++) {
			ext4_group_t group;
			ext4_grpblk_t offset;
			ext4_get_group_no_and_offset(sb, (ext4_fsblk_t)b, &group, &offset);

			struct buffer_head *bitmap_bh = ext4_read_block_bitmap(sb, group);
			// found unreadable blocks
			if (IS_ERR_OR_NULL(bitmap_bh)) {
				if (in_free_extent) break;	// assume current free extent stops right before unreadable blocks
				continue;	// otherwise, continue checking for free extents after unreadable blocks
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

		iter_info.result_block = found_start;
		iter_info.result_length = found_length;
		if (copy_to_user((void __user *)arg, &iter_info, sizeof(iter_info))) {
			return -EFAULT;
		}
		return 0;
	}
    default:
        return -ENOTTY;
    }
}