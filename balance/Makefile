CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: balance

balance: src/balance.c
	$(CC) $(CFLAGS) $< -o $@

# Internal checks only. The regression driver is tools/regress.py, run by
# `make regress`; `make test` runs that too.
test: balance
	./balance selftest --table data/numbers.txt
	python3 -I tools/regress.py --balance-bin ./balance \
		--table data/numbers.txt --baselines baselines
	sh ./test.sh

regress: balance
	python3 -I tools/regress.py --balance-bin ./balance \
		--table data/numbers.txt --baselines baselines

# Regenerate the committed baseline from the current table. Explicit only;
# `make test` and tools/regress.py never write here.
baseline: balance
	python3 -I tools/regress.py --balance-bin ./balance \
		--table data/numbers.txt --baselines baselines --write-baseline

clean:
	rm -f balance
