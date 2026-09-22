CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith
LDLIBS  += -lpthread -lm

SRC = src/sip.c src/route.c

all: ecall-bench

ecall-bench: $(SRC) src/bench.c src/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) src/bench.c $(LDLIBS)

test_router: $(SRC) tests/test_router.c src/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) tests/test_router.c $(LDLIBS)

test: test_router
	./test_router

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test

bench: ecall-bench
	./ecall-bench 200 10 5

clean:
	rm -f ecall-bench test_router

.PHONY: all test asan bench clean
