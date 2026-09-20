#   make test     stress test + real programs under LD_PRELOAD

CC      ?= gcc
CFLAGS  := -O2 -g -std=gnu11 -Wall -Wextra
BUILD   := build
LIB     := $(BUILD)/libclusteralloc.so
PRELOAD := LD_PRELOAD=$(LIB)

.PHONY: all test clean

all: $(LIB) $(BUILD)/stress

$(BUILD):
	mkdir -p $@

$(LIB): alloc/alloc.c alloc/alloc.h libc/malloc.c | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden -pthread \
	    '-DCA_API=__attribute__((visibility("default")))' \
	    -o $@ alloc/alloc.c libc/malloc.c

$(BUILD)/stress: libc/stress.c alloc/alloc.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -o $@ $<

test: $(LIB) $(BUILD)/stress
	$(BUILD)/stress
	$(PRELOAD) $(BUILD)/stress
	@echo "--- real programs under LD_PRELOAD"
	$(PRELOAD) ls -lR /usr/include > /dev/null
	seq 1 300000 | $(PRELOAD) sort -R | $(PRELOAD) sort -n | tail -1
	CLUSTERALLOC_STATS=1 $(PRELOAD) $(CC) -O2 -c alloc/alloc.c -o /dev/null

clean:
	rm -rf $(BUILD)
