# Test programs (mingw-w64). make OUT=<dir>; tests/run.sh runs them.
OUT ?= bin
CC32 = i686-w64-mingw32-gcc
CC64 = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wno-unused-function

# Every src/*.c is a 32-bit program, except the 64-bit ones in SRC64.
SRC64 := src/breakpoint-test64.c
PROGS32 := $(patsubst src/%.c,$(OUT)/%.exe,$(filter-out $(SRC64),$(wildcard src/*.c)))
HEADERS := src/aio_test.h src/d3d9-common.h
PROGS64 := $(patsubst src/%.c,$(OUT)/%.exe,$(SRC64))

LIBS = -ld3d9 -ld3dx9
LIBS_xinput-test = -lsetupapi -lhid
LIBS_recv-test = -lws2_32
LIBS_d3d11-dxmt-test = -ld3d11 -ldxgi -luuid
# Without NX_COMPAT Wine turns DEP off, as for Plutonium's bootstrapper.
LDFLAGS_lazy-exec-test = -Wl,--disable-nxcompat

all: $(PROGS32) $(PROGS64)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/%.exe: src/%.c $(HEADERS) | $(OUT)
	$(CC32) $(CFLAGS) $(LDFLAGS_$*) -DTEST_NAME=\"$*\" -o $@ $< $(or $(LIBS_$*),$(LIBS))

$(PROGS64): $(OUT)/%.exe: src/%.c $(HEADERS) | $(OUT)
	$(CC64) $(CFLAGS) -DTEST_NAME=\"$*\" -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
