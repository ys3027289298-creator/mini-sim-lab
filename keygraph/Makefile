CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: keygraph

keygraph: src/keygraph.c
	$(CC) $(CFLAGS) $< -o $@

test: keygraph
	./keygraph

clean:
	rm -f keygraph
