.PHONY:	all clean udp-bench tcp-bench

all:	udp-bench tcp-bench

udp-bench:	udp-bench.o
	$(CC) -o $@ $^ $(LDFLAGS)

tcp-bench:	tcp-bench.o
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf *.o udp-bench tcp-bench

.c.o:
	$(CC) -Wall -Wextra -std=c99 $(CFLAGS) -c -o $@ $<
