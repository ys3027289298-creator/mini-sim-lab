CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: rng

rng: src/rng.c src/rng_test.c src/rng.h
	$(CC) $(CFLAGS) src/rng.c src/rng_test.c -o $@

test: rng
	./rng

clean:
	rm -f rng
