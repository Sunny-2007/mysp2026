CC = gcc
CFLAGS = -Wall -Wextra -O2

.PHONY: all clean
all: server
server: server.c server.h
	$(CC) $(CFLAGS) server.c -o server
clean:
	rm -f server
