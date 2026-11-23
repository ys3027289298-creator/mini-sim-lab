CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: fixedstep

fixedstep: src/fixedstep.c src/fixedstep_test.c src/fixedstep.h
	$(CC) $(CFLAGS) src/fixedstep.c src/fixedstep_test.c -o $@ -lm

# pre-fix double/fmod clock, kept so the drift reproduction is runnable
fixedstep-legacy: src/fixedstep.c src/fixedstep_test.c src/fixedstep.h
	$(CC) $(CFLAGS) -DFIXEDSTEP_LEGACY src/fixedstep.c src/fixedstep_test.c -o $@ -lm

test: fixedstep
	./fixedstep

test-legacy: fixedstep-legacy
	./fixedstep-legacy

clean:
	rm -f fixedstep fixedstep-legacy
