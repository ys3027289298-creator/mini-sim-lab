CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: timeslice

timeslice: src/timeslice.c
	$(CC) $(CFLAGS) $< -o $@

test: timeslice
	./timeslice

clean:
	rm -f timeslice
