CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
DETDIR := ../det
DET_CFLAGS := -I$(DETDIR)
DET_SRCS := $(DETDIR)/detcontext.c $(DETDIR)/cli.c $(DETDIR)/evidence.c
COMMON := src/rng.c

all: rng evidence

rng: src/selftest.c $(COMMON) src/rng.h
	$(CC) $(CFLAGS) $< $(COMMON) -o $@

evidence: src/evidence.c $(COMMON) src/rng.h $(DET_SRCS)
	$(CC) $(CFLAGS) $(DET_CFLAGS) -pthread $< $(COMMON) $(DET_SRCS) -o $@

test: rng
	./rng

det: evidence
	./evidence

clean:
	rm -f rng evidence

.PHONY: all test det clean
