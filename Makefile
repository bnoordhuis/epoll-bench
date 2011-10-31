.PHONY:	all clean

all:	bench.o
	$(CC) -o bench $^ $(LDFLAGS)

clean:
	rm -rf bench bench.o

.c.o:
	$(CC) -Wall -Wextra -std=c99 $(CFLAGS) -c -o $@ $<
