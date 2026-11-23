CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS ?= -Iinclude

all: rng

rng: src/rng.c src/selftest.c include/rng.h
	$(CC) $(CFLAGS) $(CPPFLAGS) src/rng.c src/selftest.c -o $@

test: rng
	./rng

clean:
	rm -f rng
