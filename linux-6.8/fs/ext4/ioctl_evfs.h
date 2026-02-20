#include "ext4.h"

#ifndef _EXT4_IOCTL_EVFS

#define _EXT4_IOCTL_EVFS
#define EXT4_IOC_EVFS_HELLO _IO('f', 99)
#define EXT4_IOC_EVFS_BLK_ALLOC _IOW('f', 100, int)
#define EXT4_IOC_EVFS_INO_ALLOC _IOW('f', 101, int)

long ext4_evfs_entry(unsigned int cmd, struct inode * ino, struct super_block * sb, unsigned long arg);

#endif
