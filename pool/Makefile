CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS ?= -Iinclude

all: pool

pool: src/pool.c src/selftest.c include/pool.h
	$(CC) $(CFLAGS) $(CPPFLAGS) src/pool.c src/selftest.c -o $@

test: pool
	./pool

clean:
	rm -f pool
