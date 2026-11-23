CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -pthread
INC := -I../fixedstep/src -I../pool/src -I../rng/src
LIB := ../fixedstep/src/fixedstep.c ../pool/src/pool.c ../rng/src/rng.c

all: sim sim-legacy

sim: src/sim.c $(LIB)
	$(CC) $(CFLAGS) $(INC) src/sim.c $(LIB) -o $@ -lm -lpthread

# pre-fix behaviour: double/fmod clock + shared RNG + atomic-claim workers
sim-legacy: src/sim.c $(LIB)
	$(CC) $(CFLAGS) $(INC) -DSIM_LEGACY -DFIXEDSTEP_LEGACY src/sim.c $(LIB) -o $@ -lm -lpthread

# address/undefined sanitizer builds for both modes
san: src/sim.c $(LIB)
	$(CC) -O1 -g -std=c11 -Wall -Wextra -pthread -fsanitize=address,undefined $(INC) src/sim.c $(LIB) -o sim-san -lm -lpthread

san-legacy: src/sim.c $(LIB)
	$(CC) -O1 -g -std=c11 -Wall -Wextra -pthread -fsanitize=address,undefined $(INC) -DSIM_LEGACY -DFIXEDSTEP_LEGACY src/sim.c $(LIB) -o sim-san-legacy -lm -lpthread

test: sim
	./sim 1 0 /tmp/sim-smoke.rec

clean:
	rm -f sim sim-legacy sim-san sim-san-legacy
