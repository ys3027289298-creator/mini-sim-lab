CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
DETDIR := ../det
DET_CFLAGS := -I$(DETDIR)
DET_SRCS := $(DETDIR)/detcontext.c $(DETDIR)/cli.c $(DETDIR)/evidence.c
COMMON := src/pool.c

all: pool evidence

pool: src/selftest.c $(COMMON) src/pool.h
	$(CC) $(CFLAGS) $< $(COMMON) -o $@

evidence: src/evidence.c $(COMMON) src/pool.h $(DET_SRCS)
	$(CC) $(CFLAGS) $(DET_CFLAGS) -pthread $< $(COMMON) $(DET_SRCS) -o $@

test: pool
	./pool

det: evidence
	./evidence

clean:
	rm -f pool evidence

.PHONY: all test det clean
