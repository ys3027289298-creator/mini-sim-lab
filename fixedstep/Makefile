CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -pthread

all: fixedstep

fixedstep: src/fixedstep.c ../detctx/detctx.c ../detctx/detctx.h
	$(CC) $(CFLAGS) src/fixedstep.c ../detctx/detctx.c -o $@ -lm

test: fixedstep
	./fixedstep

clean:
	rm -f fixedstep
