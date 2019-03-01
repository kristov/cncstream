CC := gcc
CFLAGS := -Wall -Werror -ggdb

cncstream: cncstream.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f cncstream
