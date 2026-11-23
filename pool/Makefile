CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: pool

pool: src/pool.c src/pool_test.c src/pool.h
	$(CC) $(CFLAGS) src/pool.c src/pool_test.c -o $@

test: pool
	./pool

clean:
	rm -f pool
