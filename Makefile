# Top-level convenience targets.  Each module still builds on its own:
#   make -C fixedstep   (or pool, or rng)
CC ?= gcc

MODULES := fixedstep pool rng

all: modules evidence

modules:
	@for m in $(MODULES); do $(MAKE) -C $$m all || exit 1; done

evidence:
	@for m in $(MODULES); do $(MAKE) -C $$m evidence || exit 1; done

test:
	@for m in $(MODULES); do $(MAKE) -C $$m test || exit 1; done

det: all
	./det/verify.sh

clean:
	@for m in $(MODULES); do $(MAKE) -C $$m clean; done

.PHONY: all modules evidence test det clean
