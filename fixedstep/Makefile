CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS ?= -Iinclude

all: fixedstep

fixedstep: src/fixedstep.c src/selftest.c include/fixedstep.h
	$(CC) $(CFLAGS) $(CPPFLAGS) src/fixedstep.c src/selftest.c -o $@ -lm

test: fixedstep
	./fixedstep

clean:
	rm -f fixedstep
