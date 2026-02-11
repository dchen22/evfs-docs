#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EXT4_IOC_HELLO _IO('f', 99)

int main(void) {
    int fd = open("/home/evie/code/evfs-sandbox", O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    int ret = ioctl(fd, EXT4_IOC_HELLO);
    if (ret < 0) { perror("ioctl"); return 1; }

    printf("ioctl returned %d\n", ret);

    close(fd);

    return 0;
}