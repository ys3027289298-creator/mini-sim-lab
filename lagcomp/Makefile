CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: lagcomp

lagcomp: src/lagcomp.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: lagcomp
	./lagcomp

clean:
	rm -f lagcomp
