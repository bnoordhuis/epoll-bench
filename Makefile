CFLAGS += -Wall -Wextra -std=c99

.PHONY:	all clean

all:	bench.o
	$(CC) -o bench $^

clean:
	rm -rf bench bench.o
