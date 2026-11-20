CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: merkle

merkle: src/merkle.c
	$(CC) $(CFLAGS) $< -o $@

test: merkle
	./merkle

clean:
	rm -f merkle
