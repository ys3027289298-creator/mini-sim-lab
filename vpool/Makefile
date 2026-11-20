CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: vpool

vpool: src/vpool.c
	$(CC) $(CFLAGS) $< -o $@

test: vpool
	./vpool

asan: src/vpool.c
	$(CC) -g -O1 -std=c11 -Wall -Wextra -fsanitize=address,undefined $< -o vpool_asan
	./vpool_asan

clean:
	rm -f vpool vpool_asan
