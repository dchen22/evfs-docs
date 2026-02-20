#include <linux/types.h>
#include "evfs.h"
#include "ext4.h"
#include "ext4_jbd2.h"

/**
 * @brief journaling helper: start a journal, execute any passed in function
 * and stop the journal
 * @param args arguments to start a journal
 * @param jopb a list of bindings
 * @param len len of the list of bindings
 */
int ext4_evfs_journal(ext4_evfs_journal_args * args, ext4_evfs_jop_binding * jopb, int len) {
    handle_t * handle = ext4_journal_start_with_reserve(args->inode, args->type,
        args->blocks, args->rsv_blocks);
    if (IS_ERR(handle)) {
        return PTR_ERR(handle);
    }

    int err = 0;
    for (int i = 0; i < len; i++, jopb++) {
        if ((err = ext4_journal_get_write_access(handle, args->sb, jopb->bh, EXT4_JTR_NONE)) < 0) {
            return err;
        }

        if ((err = jopb->op(jopb->bh, jopb->op_args)) < 0) {
            return err;
        }

        if ((err = ext4_handle_dirty_metadata(handle, args->inode, jopb->bh)) < 0) {
            return err;
        }
    }

    if ((err = ext4_journal_stop(handle) < 0)) {
        return err;
    }

    return 0;
}