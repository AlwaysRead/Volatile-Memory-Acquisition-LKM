# Makefile for memdump LKM + userspace tool
#
# Targets:
#   all      - Build kernel module + dump_tool (release)
#   debug    - Build with DEBUG symbols and verbose kernel logging
#   clean    - Remove all build artifacts
#   reload   - Unload (if loaded), rebuild, and reload the module
#   info     - Print /proc/memdump_info (requires loaded module)
#   help     - Print this help

KDIR      := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
KMOD      := mem_acquire
UTOOL     := dump_tool

# Kernel module source(s)
obj-m += $(KMOD).o

# ---- Compiler flags -------------------------------------------------------

EXTRA_CFLAGS  += -Wall -Wextra -Wno-unused-parameter
EXTRA_CFLAGS  += -DMODULE_VERSION='"2.0.0"'

# User-space tool flags
CFLAGS_RELEASE := -O2 -Wall -Wextra -Wno-unused-result
CFLAGS_DEBUG   := -g -O0 -Wall -Wextra -DDEBUG -fsanitize=address
LDFLAGS        := -lssl -lcrypto

# ---- Default target -------------------------------------------------------

.PHONY: all
all: module userspace

.PHONY: module
module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

.PHONY: userspace
userspace:
	$(CC) $(CFLAGS_RELEASE) $(UTOOL).c -o $(UTOOL) $(LDFLAGS)

# ---- Debug build ----------------------------------------------------------

.PHONY: debug
debug: EXTRA_CFLAGS += -DDEBUG -g
debug:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(CC) $(CFLAGS_DEBUG) $(UTOOL).c -o $(UTOOL) $(LDFLAGS)

# ---- Clean ----------------------------------------------------------------

.PHONY: clean
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(UTOOL) memory_dump.bin memory_dump.bin.json

# ---- Convenience targets --------------------------------------------------

.PHONY: reload
reload:
	-sudo rmmod $(KMOD) 2>/dev/null; true
	$(MAKE) module
	sudo insmod $(KMOD).ko
	@echo "[+] Module loaded"
	@dmesg | tail -5

.PHONY: info
info:
	@cat /proc/memdump_info 2>/dev/null || echo "Module not loaded"

.PHONY: help
help:
	@echo ""
	@echo "  make          - Build kernel module + userspace tool (release)"
	@echo "  make debug    - Build with debug symbols"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make reload   - Unload, rebuild, reload module"
	@echo "  make info     - Print /proc/memdump_info"
	@echo ""
	@echo "  Load:    sudo insmod $(KMOD).ko [buffer_size=<bytes>] [fill_pattern=1]"
	@echo "  Acquire: sudo ./$(UTOOL) [--mmap] [--out <file>]"
	@echo "  Unload:  sudo rmmod $(KMOD)"
	@echo ""
