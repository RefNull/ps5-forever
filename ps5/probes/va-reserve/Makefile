# Large virtual-address reservation probes. Build in the release container,
# which has both toolchains at these paths.
#   va-probe.exe: run inside the title's Wine; logs to stdout and C:\va-probe.txt
#   va-probe.elf: send to the console's ELF loader; logs to the console log
PS5_PAYLOAD_SDK ?= /sources/PS5_Vulkan/.deps/native/ps5-payload-sdk
LLVM_MINGW ?= /opt/llvm-mingw
OUT ?= ../../build/va-reserve

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

MINGW_CC := $(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang
CFLAGS := -Wall -Wextra -Werror -O2

.PHONY: all clean
all: $(OUT)/va-probe.exe $(OUT)/va-probe.elf

$(OUT)/va-probe.exe: va-probe-win.c
	mkdir -p $(OUT)
	$(MINGW_CC) $(CFLAGS) -static -o $@ $<

$(OUT)/va-probe.elf: va-probe-native.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT)
