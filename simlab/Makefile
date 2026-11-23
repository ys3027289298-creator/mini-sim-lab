CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS ?= -I../fixedstep/include -I../pool/include -I../rng/include
LDLIBS ?= -lpthread -lm

all: simlab

simlab: src/simlab.c \
       ../fixedstep/src/fixedstep.c ../fixedstep/include/fixedstep.h \
       ../pool/src/pool.c ../pool/include/pool.h \
       ../rng/src/rng.c ../rng/include/rng.h
	$(CC) $(CFLAGS) $(CPPFLAGS) src/simlab.c ../fixedstep/src/fixedstep.c \
	    ../pool/src/pool.c ../rng/src/rng.c -o $@ $(LDLIBS)

test: simlab
	./simlab -t 1

clean:
	rm -f simlab
