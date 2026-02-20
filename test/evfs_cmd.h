#include <sys/ioctl.h>

#ifndef _EXT4_IOCTL_EVFS_TESTING
#define _EXT4_IOCTL_EVFS_TESTING
#define EXT4_IOC_EVFS_HELLO _IO('f', 99)
#define EXT4_IOC_EVFS_BLK_ALLOC _IOW('f', 100, int)
#define EXT4_IOC_EVFS_INO_ALLOC _IOW('f', 101, int)
#endif

