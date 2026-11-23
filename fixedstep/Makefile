CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
DETDIR := ../det
DET_CFLAGS := -I$(DETDIR)
DET_SRCS := $(DETDIR)/detcontext.c $(DETDIR)/cli.c $(DETDIR)/evidence.c
COMMON := src/fixedstep.c

all: fixedstep evidence

fixedstep: src/selftest.c $(COMMON) src/fixedstep.h
	$(CC) $(CFLAGS) $< $(COMMON) -o $@ -lm

evidence: src/evidence.c $(COMMON) src/fixedstep.h $(DET_SRCS)
	$(CC) $(CFLAGS) $(DET_CFLAGS) -pthread $< $(COMMON) $(DET_SRCS) -o $@ -lm

test: fixedstep
	./fixedstep

det: evidence
	./evidence

clean:
	rm -f fixedstep evidence

.PHONY: all test det clean
