CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: fixedstep

fixedstep: src/fixedstep.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: fixedstep
	./fixedstep

clean:
	rm -f fixedstep
