CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
LDFLAGS ?= -pthread -lm

all: emit

emit: src/emit.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

test: emit
	./emit

clean:
	rm -f emit
