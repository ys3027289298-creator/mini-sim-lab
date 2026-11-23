CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -pthread

all: rng

rng: src/rng.c ../detctx/detctx.c ../detctx/detctx.h
	$(CC) $(CFLAGS) src/rng.c ../detctx/detctx.c -o $@

test: rng
	./rng

clean:
	rm -f rng
