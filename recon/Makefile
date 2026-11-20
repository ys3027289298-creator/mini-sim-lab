CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: harness

harness: harness.c recon.c recon.h ../fixedstep/src/fixedstep.c \
         ../pool/src/pool.c ../rng/src/rng.c
	$(CC) $(CFLAGS) -I. harness.c recon.c -o $@ -lm

test: harness
	./harness

# Module self-tests recompiled with reconciliation enabled, to prove the
# instrumentation does not change what the modules compute.
module-tests:
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-unused-function -I. -DRECON ../fixedstep/src/fixedstep.c recon.c -o build/fixedstep -lm
	$(CC) $(CFLAGS) -Wno-unused-function -I. -DRECON ../pool/src/pool.c recon.c -o build/pool
	$(CC) $(CFLAGS) -Wno-unused-function -I. -DRECON ../rng/src/rng.c recon.c -o build/rng
	./build/fixedstep
	./build/pool
	./build/rng

clean:
	rm -f harness
	rm -rf build
