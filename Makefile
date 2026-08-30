CC      = gcc
CFLAGS  = -std=c17 -Wall -Wextra -Werror -O2 -g -I. -Iplatform/Linux_x86
LDFLAGS = -pthread
# Hand-written asm is IBT-compatible (endbr64) but not SHSTK-compatible.
# Force branch-only so CFLAGS -fcf-protection=full cannot stamp SHSTK onto .S.
ASFLAGS = $(CFLAGS) -fcf-protection=branch

SRCS = coroutine.c platform/Linux_x86/SystemV.c test_coroutine.c
ASMS = platform/Linux_x86/sysV.S
OBJS = $(SRCS:.c=.o) $(ASMS:.S=.o)

.PHONY: all clean test run slow normal fast turbo bench p0 p0-asan p0-tsan p1-tsan p2-tsan p3 p3-tsan p3-bench

all: test_coroutine

bench p0:
	$(MAKE) -f Makefile.p0 bench

p0-asan:
	$(MAKE) -f Makefile.p0 test-asan

p0-tsan:
	$(MAKE) -f Makefile.p0 test-tsan

p1-tsan:
	$(MAKE) -f Makefile.p1 test-tsan

p2-tsan:
	$(MAKE) -f Makefile.p2 test-tsan

p3:
	$(MAKE) -f Makefile.p3 test

p3-tsan:
	$(MAKE) -f Makefile.p3 test-tsan

p3-bench:
	$(MAKE) -f Makefile.p3 compare-pool

test_coroutine: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

platform/Linux_x86/%.o: platform/Linux_x86/%.S
	$(CC) $(ASFLAGS) -c -o $@ $<

test: test_coroutine
	./test_coroutine --speed normal

run: test

slow: test_coroutine
	./test_coroutine --speed slow

normal: test_coroutine
	./test_coroutine --speed normal

fast: test_coroutine
	./test_coroutine --speed fast

turbo: test_coroutine
	./test_coroutine --speed turbo

clean:
	rm -f $(OBJS) test_coroutine
