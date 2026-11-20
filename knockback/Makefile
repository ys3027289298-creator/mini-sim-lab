CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: knockback

knockback: src/knockback.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: knockback
	./knockback

clean:
	rm -f knockback
