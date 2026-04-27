#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Struct and ioctl definitions                                         */
/* ------------------------------------------------------------------ */

#define EXT4_NAME_LEN 255

struct ext4_evfs_extent {
    uint64_t start_block;
    uint32_t length;
};

struct ext4_evfs_inode_remap {
    uint64_t inode_number;
    struct ext4_evfs_extent *extents;
    uint32_t num_extents;
};

struct ext4_evfs_iter_freespace {
    uint64_t start_block;
    uint64_t result_block;
    uint64_t result_length;
};

#define EXT4_IOC_INODE_REMAP    _IOW('f', 108, struct ext4_evfs_inode_remap)
#define EXT4_IOC_ITER_FREESPACE _IOR('f', 106, struct ext4_evfs_iter_freespace)
#define EXT4_IOC_FLIP_BLOCK_BIT _IOW('f', 100, uint64_t)

/* ------------------------------------------------------------------ */
/* Config                                                               */
/* ------------------------------------------------------------------ */

#define SANDBOX     "/home/evie/code/evfs-sandbox"
#define TEST_DIR    SANDBOX "/test_inode_remap_c"
#define BLOCK_SIZE  4096
#define DEVICE_CMD  "df " SANDBOX " | tail -1 | awk '{print $1}'"

/* ------------------------------------------------------------------ */
/* Test state                                                           */
/* ------------------------------------------------------------------ */

static int passed = 0;
static int failed = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void check(const char *name, int condition, const char *detail) {
    if (condition) {
        printf("  [\033[92mPASS\033[0m] %s\n", name);
        passed++;
    } else {
        printf("  [\033[91mFAIL\033[0m] %s: %s\n", name, detail ? detail : "");
        failed++;
    }
}

static void get_block_device(char *out, size_t len) {
    FILE *f = popen(DEVICE_CMD, "r");
    if (!f) { perror("popen"); exit(1); }
    fgets(out, len, f);
    pclose(f);
    out[strcspn(out, "\n")] = '\0';
}

static uint64_t get_inode(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) { perror("stat"); exit(1); }
    return st.st_ino;
}

static uint64_t create_file(const char *path, const char *content, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(1); }
    write(fd, content, len);
    close(fd);
    sync();
    return get_inode(path);
}

static void drop_caches(void) {
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0) { perror("drop_caches"); return; }
    if (write(fd, "3", 1) < 0) {
        perror("write");
    }
    close(fd);
}

static int iter_freespace(int fd, uint64_t start,
                           uint64_t *result_block, uint64_t *result_length) {
    struct ext4_evfs_iter_freespace info = { .start_block = start };
    if (ioctl(fd, EXT4_IOC_ITER_FREESPACE, &info) < 0)
        return -1;
    *result_block  = info.result_block;
    *result_length = info.result_length;
    return 0;
}

static int flip_block_bit(int fd, uint64_t block) {
    return ioctl(fd, EXT4_IOC_FLIP_BLOCK_BIT, &block);
}

/* Find count consecutive free blocks and flip their bits to allocate them */
static uint64_t allocate_free_blocks(int fd, uint32_t count) {
    uint64_t cursor = 0;
    for (;;) {
        uint64_t rb, rl;
        if (iter_freespace(fd, cursor, &rb, &rl) < 0) return 0;
        if (rb == 0 && rl == 0) return 0;
        if (rl >= count) {
            for (uint32_t i = 0; i < count; i++)
                flip_block_bit(fd, rb + i);
            return rb;
        }
        cursor = rb + rl;
    }
}

/* Flip allocated blocks back to free */
static void free_blocks(int fd, uint64_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        flip_block_bit(fd, start + i);
}

static int do_inode_remap(int fd, uint64_t inode_number,
                           struct ext4_evfs_extent *extents, uint32_t num_extents) {
    struct ext4_evfs_inode_remap req = {
        .inode_number = inode_number,
        .extents      = extents,
        .num_extents  = num_extents,
    };
    return ioctl(fd, EXT4_IOC_INODE_REMAP, &req);
}

static void write_block_dev(const char *device, uint64_t block_num,
                             const uint8_t *data, size_t size) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "dd of=%s bs=%d seek=%lu count=1 status=none",
             device, BLOCK_SIZE, block_num);
    FILE *f = popen(cmd, "w");
    if (!f) { perror("popen dd write"); return; }
    fwrite(data, 1, size, f);
    fflush(f);
    pclose(f);
}

/* Returns list of physical blocks for an inode via debugfs. */
static int debugfs_extents(uint64_t ino, const char *device,
                            uint64_t *blocks, int max_blocks) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "debugfs -R 'stat <%lu>' %s 2>/dev/null", ino, device);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;

    char line[512];
    int in_extents = 0;
    int count = 0;

    while (fgets(line, sizeof(line), f) && count < max_blocks) {
        if (strstr(line, "EXTENTS") || strstr(line, "Extents")) {
            in_extents = 1;
            continue;
        }
        if (!in_extents) continue;
        if (line[0] == '\n') break;

        char *colon = strrchr(line, ':');
        if (colon) {
            uint64_t phys = strtoull(colon + 1, NULL, 10);
            if (phys > 0)
                blocks[count++] = phys;
        }
    }
    pclose(f);
    return count;
}

/* Remove a single file; safe to call if file doesn't exist */
static void remove_file(const char *path) {
    unlink(path);
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

static void test_remap_single_extent_content_visible(int fd, const char *device) {
    printf("\ntest_remap_single_extent_content_visible\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/remap_content.txt", TEST_DIR);
    uint64_t ino = create_file(path, "original", 20);

    printf("Created file with inode number = %lu\n", ino);

    /* allocate_free_blocks already calls flip_block_bit on each block,
       so the new block is marked as used in the bitmap before remap */
    uint64_t block = allocate_free_blocks(fd, 1);
    if (!block) {
        printf("  [SKIP] no free blocks\n");
        remove_file(path);
        return;
    }

    /* write known pattern directly to the new block */
    uint8_t pattern[BLOCK_SIZE] = {0};
    memcpy(pattern, "EVFS_REMAP_T3ST_CONTENT", 23);
    write_block_dev(device, block, pattern, BLOCK_SIZE);
    sync();

    printf("filefrag before remap:\n");
    char ffcmd[524];
    snprintf(ffcmd, sizeof(ffcmd), "filefrag -v %s", path);
    system(ffcmd);
    printf("block we remapped to: %lu\n", block);

    struct ext4_evfs_extent ext = { .start_block = block, .length = 1 };
    // struct ext4_evfs_inode_remap remap_info = {
    //     .inode_number = ino,
    //     .extents = &ext,
    //     .num_extents = 1
    // };
    if (do_inode_remap(fd, ino, &ext, 1) < 0) {
    // if (ioctl(fd, EXT4_IOC_INODE_REMAP, &remap_info) < 0) {
        perror("inode_remap");
        free_blocks(fd, block, 1);
        remove_file(path);
        return;
    }

    int tmp = open(path, O_RDONLY);
    fsync(tmp);
    close(tmp);

    sync();
    drop_caches();

    printf("Sleeping for 2 seconds...\n");
    sleep(2);

    struct stat st;
    stat(path, &st);
    printf("file size after remap: %ld\n", st.st_size);

    printf("filefrag after remap:\n");
    memset(ffcmd, 0, 524);
    snprintf(ffcmd, sizeof(ffcmd), "filefrag -v %s", path);
    system(ffcmd);
    printf("block we remapped to: %lu\n", block);

    printf("block contents BEFORE file read:\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "dd if=%s bs=4096 skip=%lu count=1 2>/dev/null | head -c 30 | xxd",
            device, block);
    system(cmd);

    int rfd = open(path, O_RDONLY);
    char buf[24] = {0};
    read(rfd, buf, 23);
    close(rfd);

    printf("Contents of buf:\n");
    for (int i = 0; i < 24; i++) {
        printf("buf[%d] = %c\n", i, buf[i]);
    }

    check("remapped file contains new block content",
          memcmp(buf, "EVFS_REMAP_T3ST_CONTENT", 23) == 0, buf);

    /* clean up file */
    remove_file(path);
}

static void test_remap_changes_physical_blocks(int fd, const char *device) {
    printf("\ntest_remap_changes_physical_blocks\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/remap_blocks.txt", TEST_DIR);
    char original[BLOCK_SIZE];
    memset(original, 'x', BLOCK_SIZE);
    uint64_t ino = create_file(path, original, BLOCK_SIZE);

    uint64_t old_blocks[16];
    int old_count = debugfs_extents(ino, device, old_blocks, 16);
    if (!old_count) {
        printf("  [SKIP] could not get original blocks\n");
        remove_file(path);
        return;
    }

    uint64_t block = allocate_free_blocks(fd, 1);
    if (!block) {
        printf("  [SKIP] no free blocks\n");
        remove_file(path);
        return;
    }

    struct ext4_evfs_extent ext = { .start_block = block, .length = 1 };
    if (do_inode_remap(fd, ino, &ext, 1) < 0) {
        perror("inode_remap");
        free_blocks(fd, block, 1);
        remove_file(path);
        return;
    }
    drop_caches();

    uint64_t new_blocks[16];
    int new_count = debugfs_extents(ino, device, new_blocks, 16);

    int found_new = 0;
    for (int i = 0; i < new_count; i++)
        if (new_blocks[i] == block) found_new = 1;
    check("new physical block present after remap", found_new, "");

    int old_still_present = 0;
    for (int o = 0; o < old_count; o++)
        for (int n = 0; n < new_count; n++)
            if (old_blocks[o] == new_blocks[n]) old_still_present = 1;
    check("old physical blocks no longer present", !old_still_present, "");

    remove_file(path);
}

static void test_remap_old_blocks_become_free(int fd, const char *device) {
    printf("\ntest_remap_old_blocks_become_free\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/remap_free.txt", TEST_DIR);
    char original[BLOCK_SIZE];
    memset(original, 'y', BLOCK_SIZE);
    uint64_t ino = create_file(path, original, BLOCK_SIZE);

    uint64_t old_blocks[16];
    int old_count = debugfs_extents(ino, device, old_blocks, 16);
    if (!old_count) {
        printf("  [SKIP] could not get original blocks\n");
        remove_file(path);
        return;
    }

    uint64_t block = allocate_free_blocks(fd, 1);
    if (!block) {
        printf("  [SKIP] no free blocks\n");
        remove_file(path);
        return;
    }

    struct ext4_evfs_extent ext = { .start_block = block, .length = 1 };
    if (do_inode_remap(fd, ino, &ext, 1) < 0) {
        perror("inode_remap");
        free_blocks(fd, block, 1);
        remove_file(path);
        return;
    }
    sync();
    drop_caches();

    for (int o = 0; o < old_count; o++) {
        int found_free = 0;
        uint64_t cursor = 0;
        for (int iter = 0; iter < 100000; iter++) {
            uint64_t rb, rl;
            if (iter_freespace(fd, cursor, &rb, &rl) < 0) break;
            if (rb == 0 && rl == 0) break;
            if (old_blocks[o] >= rb && old_blocks[o] < rb + rl) {
                found_free = 1;
                break;
            }
            cursor = rb + rl;
        }
        char detail[64];
        snprintf(detail, sizeof(detail), "block %lu not free", old_blocks[o]);
        check("old block is now free", found_free, detail);
    }

    remove_file(path);
}

static void test_remap_multiple_extents(int fd, const char *device) {
    printf("\ntest_remap_multiple_extents\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/remap_multi.txt", TEST_DIR);
    char original[BLOCK_SIZE];
    memset(original, 'z', BLOCK_SIZE);
    uint64_t ino = create_file(path, original, BLOCK_SIZE);

    uint64_t block_a = allocate_free_blocks(fd, 1);
    if (!block_a) {
        printf("  [SKIP] no free blocks\n");
        remove_file(path);
        return;
    }
    uint64_t block_b = allocate_free_blocks(fd, 1);
    if (!block_b) {
        free_blocks(fd, block_a, 1);
        remove_file(path);
        printf("  [SKIP] could not allocate second block\n");
        return;
    }

    struct ext4_evfs_extent exts[2] = {
        { .start_block = block_a, .length = 1 },
        { .start_block = block_b, .length = 1 },
    };
    if (do_inode_remap(fd, ino, exts, 2) < 0) {
        perror("inode_remap");
        free_blocks(fd, block_a, 1);
        free_blocks(fd, block_b, 1);
        remove_file(path);
        return;
    }
    drop_caches();

    uint64_t new_blocks[16];
    int new_count = debugfs_extents(ino, device, new_blocks, 16);

    int found_a = 0, found_b = 0;
    for (int i = 0; i < new_count; i++) {
        if (new_blocks[i] == block_a) found_a = 1;
        if (new_blocks[i] == block_b) found_b = 1;
    }
    check("first remapped block present",  found_a, "");
    check("second remapped block present", found_b, "");

    remove_file(path);
}

static void test_remap_updates_file_size(int fd, const char *device) {
    printf("\ntest_remap_updates_file_size\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/remap_size.txt", TEST_DIR);
    char original[BLOCK_SIZE];
    memset(original, 's', BLOCK_SIZE);
    uint64_t ino = create_file(path, original, BLOCK_SIZE);

    uint64_t block = allocate_free_blocks(fd, 2);
    if (!block) {
        printf("  [SKIP] no free blocks\n");
        remove_file(path);
        return;
    }

    struct ext4_evfs_extent ext = { .start_block = block, .length = 2 };
    if (do_inode_remap(fd, ino, &ext, 1) < 0) {
        perror("inode_remap");
        free_blocks(fd, block, 2);
        remove_file(path);
        return;
    }
    drop_caches();

    struct stat st;
    stat(path, &st);
    char detail[64];
    snprintf(detail, sizeof(detail), "expected %d got %ld",
             2 * BLOCK_SIZE, st.st_size);
    check("file size updated to 2 blocks",
          st.st_size == 2 * BLOCK_SIZE, detail);

    remove_file(path);
}

static void test_remap_invalid_inode_returns_error(int fd, const char *device) {
    printf("\ntest_remap_invalid_inode_returns_error\n");

    uint64_t block = allocate_free_blocks(fd, 1);
    if (!block) { printf("  [SKIP] no free blocks\n"); return; }

    struct ext4_evfs_extent ext = { .start_block = block, .length = 1 };
    int ret = do_inode_remap(fd, 999999999ULL, &ext, 1);
    check("invalid inode returns error", ret < 0,
          "expected error but got success");

    /* remap failed, block we claimed is still marked used in bitmap — free it */
    free_blocks(fd, block, 1);
}

static void test_remap_zero_extents_returns_error(int fd, const char *device) {
    printf("\ntest_remap_zero_extents_returns_error\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/remap_zero.txt", TEST_DIR);
    uint64_t ino = create_file(path, "test", 4);

    int ret = do_inode_remap(fd, ino, NULL, 0);
    check("zero extents returns error", ret < 0,
          "expected EINVAL but got success");

    remove_file(path);
}

static void test_remap_content_of_multiple_extents(int fd, const char *device) {
    printf("\ntest_remap_content_of_multiple_extents\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/remap_multi_content.txt", TEST_DIR);
    char original[BLOCK_SIZE];
    memset(original, 'o', BLOCK_SIZE);
    uint64_t ino = create_file(path, original, BLOCK_SIZE);

    uint64_t block_a = allocate_free_blocks(fd, 1);
    if (!block_a) {
        printf("  [SKIP] no free blocks\n");
        remove_file(path);
        return;
    }
    uint64_t block_b = allocate_free_blocks(fd, 1);
    if (!block_b) {
        free_blocks(fd, block_a, 1);
        remove_file(path);
        printf("  [SKIP] could not allocate second block\n");
        return;
    }

    uint8_t content_a[BLOCK_SIZE] = {0};
    uint8_t content_b[BLOCK_SIZE] = {0};
    memcpy(content_a, "BLOCK_A_CONTENT", 15);
    memcpy(content_b, "BLOCK_B_CONTENT", 15);
    write_block_dev(device, block_a, content_a, BLOCK_SIZE);
    write_block_dev(device, block_b, content_b, BLOCK_SIZE);
    sync();

    struct ext4_evfs_extent exts[2] = {
        { .start_block = block_a, .length = 1 },
        { .start_block = block_b, .length = 1 },
    };
    if (do_inode_remap(fd, ino, exts, 2) < 0) {
        perror("inode_remap");
        free_blocks(fd, block_a, 1);
        free_blocks(fd, block_b, 1);
        remove_file(path);
        return;
    }
    drop_caches();

    int rfd = open(path, O_RDONLY);
    uint8_t data[2 * BLOCK_SIZE];
    read(rfd, data, sizeof(data));
    close(rfd);

    check("first block content correct",
          memcmp(data, "BLOCK_A_CONTENT", 15) == 0,
          (char *)data);
    check("second block content correct",
          memcmp(data + BLOCK_SIZE, "BLOCK_B_CONTENT", 15) == 0,
          (char *)(data + BLOCK_SIZE));

    remove_file(path);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: must run as root (sudo)\n");
        return 1;
    }

    mkdir(TEST_DIR, 0755);

    int fd = open(SANDBOX, O_RDONLY);
    if (fd < 0) { perror("open sandbox"); return 1; }

    char device[256];
    get_block_device(device, sizeof(device));

    test_remap_single_extent_content_visible(fd, device);
    // test_remap_changes_physical_blocks(fd, device);
    // test_remap_old_blocks_become_free(fd, device);
    // test_remap_multiple_extents(fd, device);
    // test_remap_updates_file_size(fd, device);
    // test_remap_invalid_inode_returns_error(fd, device);
    // test_remap_zero_extents_returns_error(fd, device);
    // test_remap_content_of_multiple_extents(fd, device);

    close(fd);

    /* remove only the test directory we created, not anything else in sandbox */
    rmdir(TEST_DIR);

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}