CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: clocksync

clocksync: src/clocksync.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: clocksync
	./clocksync

clean:
	rm -f clocksync
