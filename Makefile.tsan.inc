# Shared TSan flags for Makefile.p0 / p1 / p2 / p3.
# Ubuntu 20.04 gcc-9 often ships without libtsan_preinit.o; inject an empty
# stub on the link path. Newer gcc (CI ubuntu-latest) finds the real file.

TSAN_CFLAGS = -fsanitize=thread -fno-omit-frame-pointer

TSAN_PREINIT_DIR := $(abspath .tsan-preinit)
TSAN_PREINIT_SYS := $(shell $(CC) -print-file-name=libtsan_preinit.o 2>/dev/null)

.PHONY: tsan-preinit

ifeq ($(TSAN_PREINIT_SYS),libtsan_preinit.o)
# -B makes gcc search this prefix for libtsan_preinit.o (%s in the spec).
TSAN_LDFLAGS = -fsanitize=thread -B$(TSAN_PREINIT_DIR)/

tsan-preinit:
	mkdir -p $(TSAN_PREINIT_DIR)
	echo '/* stub for missing libtsan_preinit.o */' | \
		$(CC) -c -x c - -o $(TSAN_PREINIT_DIR)/libtsan_preinit.o
else
TSAN_LDFLAGS = -fsanitize=thread

tsan-preinit:
	@true
endif
