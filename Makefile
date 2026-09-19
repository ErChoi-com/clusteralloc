CC      ?= gcc
CFLAGS  := -O2 -g -std=gnu11 -Wall -Wextra
BUILD   := build
LIB     := $(BUILD)/libclusteralloc.so

.PHONY: all clean

all: $(LIB)

$(BUILD):
	mkdir -p $@

$(LIB): alloc/alloc.c alloc/alloc.h libc/malloc.c | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden -pthread \
	    '-DCA_API=__attribute__((visibility("default")))' \
	    -o $@ alloc/alloc.c libc/malloc.c

clean:
	rm -rf $(BUILD)
