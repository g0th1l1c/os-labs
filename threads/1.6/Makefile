CC = gcc
CFLAGS = -Wall -Wextra -std=c11

all: libmythread.a example

mythread.o: mythread.c mythread.h
	$(CC) $(CFLAGS) -c mythread.c

libmythread.a: mythread.o
	ar rcs $@ $^

example: example.c libmythread.a
	$(CC) $(CFLAGS) example.c -L. -lmythread -o $@

clean:
	rm -f mythread.o libmythread.a example

.PHONY: all clean
