CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: gacha

gacha: src/gacha.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: gacha
	./gacha

clean:
	rm -f gacha
