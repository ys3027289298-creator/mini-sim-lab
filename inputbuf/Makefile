CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: inputbuf

inputbuf: src/inputbuf.c
	$(CC) $(CFLAGS) $< -o $@

test: inputbuf
	./inputbuf

csv: inputbuf
	./inputbuf ../artifacts/inputbuf_hit_curve.csv

clean:
	rm -f inputbuf
