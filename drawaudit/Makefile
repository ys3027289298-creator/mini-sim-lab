CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: drawaudit

drawaudit: src/drawaudit.c
	$(CC) $(CFLAGS) -DAUDIT_ENABLED=1 $< -o $@

drawaudit_off: src/drawaudit.c
	$(CC) $(CFLAGS) -DAUDIT_ENABLED=0 $< -o $@

test: drawaudit drawaudit_off
	./drawaudit > .on.log && cat .on.log
	./drawaudit_off > .off.log && cat .off.log
	@on=$$(sed -n 's/^work_ns=//p' .on.log); \
	off=$$(sed -n 's/^work_ns=//p' .off.log); \
	awk -v on="$$on" -v off="$$off" 'BEGIN { \
		pct = (on - off) * 100.0 / on; \
		printf "audit_overhead_pct=%.3f\n", pct; \
		exit (pct <= 2.0) ? 0 : 1 }'
	@rm -f .on.log .off.log

clean:
	rm -f drawaudit drawaudit_off
