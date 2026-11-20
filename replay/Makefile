CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: replay

replay: src/replay.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: replay
	sh ./test.sh

# Regenerate the committed sample recordings and baselines. Explicit only;
# `make test` never touches them.
samples: replay
	mkdir -p recordings baselines
	for s in 1 2 3 4; do \
		./replay gen $$s 300 recordings/run$$s.rec && \
		./replay record recordings/run$$s.rec baselines/run$$s.traj; \
	done

clean:
	rm -f replay
