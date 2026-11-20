CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: pool

pool: src/pool.c
	$(CC) $(CFLAGS) $< -o $@ 

test: pool
	./pool

clean:
	rm -f pool
