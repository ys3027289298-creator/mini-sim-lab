CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -pthread

all: pool

pool: src/pool.c ../detctx/detctx.c ../detctx/detctx.h
	$(CC) $(CFLAGS) src/pool.c ../detctx/detctx.c -o $@

test: pool
	./pool

clean:
	rm -f pool
