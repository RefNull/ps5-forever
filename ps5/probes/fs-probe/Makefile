# File-operation latency probe. Build in the release container, which has
# llvm-mingw at this path.
#   fs-probe.exe: run inside the title's Wine; logs to stdout and C:\fs-probe.txt
LLVM_MINGW ?= /opt/llvm-mingw
OUT ?= ../../build/fs-probe

MINGW_CC := $(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang
CFLAGS := -Wall -Wextra -Werror -O2

.PHONY: all clean
all: $(OUT)/fs-probe.exe

$(OUT)/fs-probe.exe: fs-probe-win.c
	mkdir -p $(OUT)
	$(MINGW_CC) $(CFLAGS) -static -o $@ $<

clean:
	rm -rf $(OUT)
