CC      = gcc
CFLAGS  = -O2 -Wall -Wextra

.PHONY: all clean run

all: fork_bench

fork_bench: fork_bench.c
	$(CC) $(CFLAGS) -o $@ $<

run: fork_bench
	./fork_bench

clean:
	rm -f fork_bench
