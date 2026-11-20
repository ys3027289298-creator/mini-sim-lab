CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: rng

rng: src/rng.c
	$(CC) $(CFLAGS) $< -o $@ 

test: rng
	./rng

clean:
	rm -f rng
