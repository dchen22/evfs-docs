#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#define BUF_CAPACITY 256

int main() {
    /*
    fd = open file b.txt
    pread b.txt once
    rename a.txt -> b.txt
    pread b.txt again
    do both preads give the same result?
    */
    int err;
    char * bname;
    char * aname;
    int fd; 

    bname = "/home/evie/code/evfs-sandbox/b.txt";
    aname = "/home/evie/code/evfs-sandbox/a.txt";

    fd = open(bname, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    char buf[BUF_CAPACITY];
    memset(buf, '\0', BUF_CAPACITY);

    // read b.txt for the first time
    pread(fd, buf, BUF_CAPACITY, 0);
    printf("First read from b.txt:\n");
    printf("%.*s\n", BUF_CAPACITY, buf);

    // rename b.txt to a.txt
    err = rename(bname, aname);
    if (err) {
        perror("rename");
        close(fd);
        return -1;
    }

    // reset buffer
    memset(buf, '\0', BUF_CAPACITY);

    // read again
    pread(fd, buf, BUF_CAPACITY, 0);
    printf("Read from a.txt (used to be b.txt):\n");
    printf("%.*s\n", BUF_CAPACITY, buf);

    close(fd);
}