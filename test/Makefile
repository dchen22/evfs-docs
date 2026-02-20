CC = gcc
CFLAGS = -g

.PHONY: all clean

all: test-hello

test-hello: test-hello.o
	$(CC) -o $@ $(CFLAGS) $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf *.o test-hello

