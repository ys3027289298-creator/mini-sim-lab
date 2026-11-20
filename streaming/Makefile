CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: streaming

streaming: src/streaming.c
	$(CC) $(CFLAGS) $< -o $@

test: streaming
	./streaming

clean:
	rm -f streaming
