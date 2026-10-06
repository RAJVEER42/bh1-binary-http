CC     ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic

all: bserve bcurl

bserve: src/bserve.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bserve.c src/proto.c

bcurl: src/bcurl.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bcurl.c src/proto.c

test: all
	python3 tests/conformance.py

clean:
	rm -f bserve bcurl

.PHONY: all test clean
