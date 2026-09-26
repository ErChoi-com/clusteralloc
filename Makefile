# Build and run everything from Linux (or WSL):
#
#   make test     malloc replacement: stress test + real programs under LD_PRELOAD
#   make bench    pipeline latency, system malloc vs clusteralloc
#   make run      boot the latency-lab kernel in QEMU

CC      ?= gcc
CFLAGS  := -O2 -g -std=gnu11 -Wall -Wextra
BUILD   := build
LIB     := $(BUILD)/libclusteralloc.so
PRELOAD := LD_PRELOAD=$(LIB)

KERNEL_ELF := kernel/target/x86_64-unknown-none/release/kernel
KVM        := $(shell test -w /dev/kvm && echo -enable-kvm -cpu host)
QEMU       := qemu-system-x86_64 $(KVM) -m 256M -display none -serial stdio -no-reboot \
              -device isa-debug-exit,iobase=0xf4,iosize=0x04

.PHONY: all test bench kernel run clean

all: $(LIB) $(BUILD)/stress $(BUILD)/pipeline kernel

$(BUILD):
	mkdir -p $@

$(LIB): alloc/alloc.c alloc/alloc.h libc/malloc.c | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden -pthread \
	    '-DCA_API=__attribute__((visibility("default")))' \
	    -o $@ alloc/alloc.c libc/malloc.c

$(BUILD)/stress: libc/stress.c alloc/alloc.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -o $@ $<

$(BUILD)/pipeline: bench/linux.c bench/pipeline.c bench/pipeline.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -o $@ bench/linux.c bench/pipeline.c

test: $(LIB) $(BUILD)/stress
	$(BUILD)/stress
	$(PRELOAD) $(BUILD)/stress
	@echo "--- real programs under LD_PRELOAD"
	$(PRELOAD) ls -lR /usr/include > /dev/null
	seq 1 300000 | $(PRELOAD) sort -R | $(PRELOAD) sort -n | tail -1
	CLUSTERALLOC_STATS=1 $(PRELOAD) $(CC) -O2 -c alloc/alloc.c -o /dev/null

bench: $(LIB) $(BUILD)/pipeline
	$(BUILD)/pipeline
	$(PRELOAD) $(BUILD)/pipeline

kernel: | $(BUILD)
	cd kernel && cargo build --release
	objcopy -O binary $(KERNEL_ELF) $(BUILD)/kernel.bin

# isa-debug-exit turns the kernel's exit code into (code << 1) | 1; 33 = success.
run: kernel
	$(QEMU) -kernel $(BUILD)/kernel.bin; status=$$?; [ $$status -eq 33 ]

clean:
	rm -rf $(BUILD)
	cd kernel && cargo clean
