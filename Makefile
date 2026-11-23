# Top-level convenience targets. Each module still builds standalone.
SUBDIRS := detctx fixedstep pool rng

.PHONY: all test clean $(SUBDIRS)

all test clean:
	@for d in fixedstep pool rng; do $(MAKE) -C $$d $@ || exit 1; done
