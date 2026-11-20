CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: target

target: src/target.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: target
	./target

clean:
	rm -f target
