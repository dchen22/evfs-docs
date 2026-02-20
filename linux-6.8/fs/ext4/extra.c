/** TODO: modify this for inode allocation
 * @brief journal a inode allocation (?to an extent).
 * Note that this function alone is now creating a temporary
 * inconsistency because there's a leak inode (i.e. not
 * being used by any inode)
 * 
 * WARNING: TESTING ONLY; Do NOT use this on a system fs.
 * 
 * @param ino pointer to the in-memory the file inode
 * @param sb pointer to the in-memory superblock
 * @param iindex the index of inode to allocate
 * @return 0 on success otherwise error code:
 *   * -EEXIST: already allocated;
 *   * -EINVAL: fail to load group descriptor. either rcu deference fails
 *     or group index out of range;
 *   * -ERRNO: errors from ext4 functions;
 */
long evfs_journal_inode_alloc(struct inode * ino, struct super_block * sb, ino_t iindex) {
    ext4_group_t grp_i = iindex / EXT4_INODES_PER_GROUP(sb);
    ino_t offset = iindex % EXT4_INODES_PER_GROUP(sb);

    struct buffer_head * bh_ibmap = ext4_read_inode_bitmap(sb, grp_i);
    if (IS_ERR(bh_ibmap)) {
        brelse(bh_ibmap); // release the buffer head
        return PTR_ERR(bh_ibmap); // error from ext4_read_inode_bitmap
    }

    // read the group descriptor
    struct buffer_head * bh_gdp;
    struct ext4_group_desc * gdp = ext4_get_group_desc(sb, grp_i, &bh_gdp);

    if (!gdp) {
        brelse(bh_ibmap);
        return -EINVAL;
    }

    // get the inode block
    u64 ino_tbl_i = gdp->bg_inode_table_hi << 32 | gdp->bg_inode_table_lo;
    u64 ino_blk_i = ino_tbl_i + offset / EXT4_SB(sb)->s_inodes_per_block;
    u64 ino_i = offset % EXT4_SB(sb)->s_inodes_per_block;
    struct buffer_head * bh_itb = ext4_sb_bread(sb, ino_blk_i, REQ_OP_READ);
    if (IS_ERR(bh_itb)) {
        brelse(bh_ibmap);
        brelse(bh_gdp); // release both buffer head
        return PTR_ERR(bh_itb);
    }

    handle_t * handle = ext4_journal_start_with_reserve(ino, EXT4_HT_MAP_BLOCKS, 2, 0);
    int err = 0, tmperr;

    // inode bitmap: turn the corresponding inode bitmap to 1
    if ((tmperr = ext4_journal_get_write_access(handle, sb, bh_ibmap, EXT4_JTR_NONE)) < 0) {
        err = tmperr;
        goto __evfs_journal_inode_alloc_out;
    }

    if (ext4_test_and_set_bit(iindex, &(bh_ibmap->b_state))) {
        err = -EEXIST;
        goto __evfs_journal_inode_alloc_out;
    }
    
    ext4_block_bitmap_csum_set(sb, gdp, bh_ibmap);

    if ((tmperr = ext4_handle_dirty_metadata(handle, ino, bh_ibmap)) < 0) {
        err = tmperr;
        goto __evfs_journal_inode_alloc_out;
    }

    // TODO: modify the in-disk inode
    if ((tmperr = ext4_journal_get_write_access(handle, sb, bh_ibmap, EXT4_JTR_NONE)) < 0) {
        err = tmperr;
        goto __evfs_journal_inode_alloc_out;
    }

    // TODO: ...

    if ((tmperr = ext4_handle_dirty_metadata(handle, ino, bh_ibmap)) < 0) {
        err = tmperr;
        goto __evfs_journal_inode_alloc_out;
    }

    // modify the metadata in the block group descriptor
    if ((tmperr = ext4_journal_get_write_access(handle, sb, bh_gdp, EXT4_JTR_NONE)) < 0) {
        err = tmperr;
        goto __evfs_journal_inode_alloc_out;
    }

    // modify the free block count
    u32 free_inodes_count = gdp->bg_free_inodes_count_hi << 16 | gdp->bg_free_inodes_count_lo;
    free_inodes_count -= 1;
    gdp->bg_free_inodes_count_hi = (u16) (free_inodes_count >> 16);
    gdp->bg_free_inodes_count_lo = (u16) (free_inodes_count & ~(0xFFFF << 16));

    // modify the checksum
    ext4_group_desc_csum_set(sb, grp_i, gdp);

    if ((tmperr = ext4_handle_dirty_metadata(handle, ino, bh_gdp)) < 0) {
        err = tmperr;
        goto __evfs_journal_inode_alloc_out;
    }

    if ((tmperr = ext4_journal_stop(handle)) < 0) {
        err = tmperr;
        goto __evfs_journal_inode_alloc_out;
    }

__evfs_journal_inode_alloc_out:
    brelse(bh_ibmap);
    brelse(bh_gdp);
    return (long) err;
}
