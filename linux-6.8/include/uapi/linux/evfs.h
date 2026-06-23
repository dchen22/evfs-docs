#include <linux/types.h>

#ifndef _GENERIC_EVFS
#define _GENERIC_EVFS

struct evfs_ino_read_out
{
	unsigned long est_dev;
	unsigned long est_ino;
	unsigned int est_mode;
	unsigned int est_nlink;
	unsigned int est_uid;
	unsigned int est_gid;
	unsigned long est_rdev;
	unsigned long est_version;
	long est_size;
	int est_blksize;
	int __pad1;
	long est_blocks;
	long est_atime;
	unsigned long est_atime_nsec;
	long est_mtime;
	unsigned long est_mtime_nsec;
	long est_ctime;
	unsigned long est_ctime_nsec;
	unsigned int __unused4;
	unsigned int __unused5;
};

struct evfs_ino_read_args
{
	struct {
		unsigned long iindex;
	} in;
	struct evfs_ino_read_out out;
};

typedef struct evfs_ino_read_out evfs_stat;

#endif

#ifndef _EXT4_EVFS
#define _EXT4_EVFS

#ifndef EXT4_NAME_LEN
#define EXT4_NAME_LEN 255
#endif

// dentry operation params and/or return values
struct ext4_evfs_de_add_args
{
	struct {
		unsigned long parent_ino_num;
		unsigned long child_ino_num;
		char name[EXT4_NAME_LEN];	// EXT4_NAME_LEN = 255
	} in;
	struct {} out;
};

struct ext4_evfs_de_read_args
{
	struct {
		unsigned long dir_ino_num;	// inode to read dentries of
		__u32 target_dentry_index;	// ith dentry to get
	} in;
	struct {
		unsigned long ino_num;	// inode num 
		__u8 file_type;
		__u8 name_len;
		char name[EXT4_NAME_LEN];
	} out;
};

struct ext4_evfs_de_delete_args
{
	struct {
		unsigned long dir_ino_num;	// inode to read dentries of
		char name[EXT4_NAME_LEN];	// name of dentry to delete
	} in;
	struct {} out;
};

struct ext4_evfs_de_update_args
{
	struct {
		unsigned long dir_ino_num;
		__u32 target_dentry_index;
		unsigned long new_ino_num;
	} in;
	struct {} out;
};

struct ext4_evfs_ino_iter_args
{
	struct {
		unsigned long start;	// starting inode to iterate from
	} in;
	struct {
		unsigned long ino_num;  	// next active inode number
	} out;
};

struct ext4_evfs_fsp_iter_args
{
	struct {
		unsigned long long start;	// some block in data section ("start block")
	} in;
	struct {
		unsigned long long block;	// next free block after start block
		unsigned long long length;	// how many free blocks occur consecutively
	} out;
};

struct ext4_evfs_ext
{
	unsigned int log_start;
	unsigned long long phy_start;
	unsigned int len;
};

struct ext4_evfs_ext_read_args
{
	struct {
		unsigned long ino_num;
		unsigned int max_num_exts;
	} in;
	struct  {
		struct ext4_evfs_ext * exts;
		unsigned int num_exts;
	} out;
};

struct ext4_evfs_ext_mv_args
{
	struct {
		unsigned long ino_num;
		unsigned long long exp_iver;
		struct ext4_evfs_ext ext;
	} in;
	struct {
		unsigned int num_exts; 
		struct ext4_evfs_ext * exts;
	} out;
	
};

#define EXT4_EVFS_HELLO _IO('f', 99)
#define EXT4_EVFS_IVER _IOR('f', 100, unsigned long long)
#define EXT4_EVFS_BLK_ALLOC _IO('f', 101)
#define EXT4_EVFS_BLK_FREE _IO('f', 102)
#define EXT4_EVFS_INO_ALLOC _IO('f', 103)
#define EXT4_EVFS_INO_FREE _IO('f', 104)
#define EXT4_EVFS_INO_READ _IOWR('f', 105, struct evfs_ino_read_args)
#define EXT4_EVFS_DEN_ADD _IOW('f', 106, struct ext4_evfs_de_add_args)
#define EXT4_EVFS_DEN_READ _IOWR('f', 107, struct ext4_evfs_de_read_args)
#define EXT4_EVFS_DEN_DELETE _IOW('f', 108, struct ext4_evfs_de_delete_args)
#define EXT4_EVFS_DEN_UPDATE _IOW('f', 109, struct ext4_evfs_de_update_args)
#define EXT4_EVFS_EXT_READ _IOWR('f', 110, struct ext4_evfs_ext_read_args)
#define EXT4_EVFS_EXT_MV _IOWR('f', 111, struct ext4_evfs_ext_mv_args)

#endif

