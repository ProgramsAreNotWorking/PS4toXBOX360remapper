# ds4xbox - Makefile
#
# KERNEL_HEADERS is optional.  It is useful on systems where the kernel
# headers are not installed under /usr/include (for example NixOS):
#
#   make KERNEL_HEADERS=/nix/store/...-linux-headers-.../include

CC ?= cc
PKG_CONFIG ?= pkg-config

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

KERNEL_HEADERS ?=
OBJDIR ?= build

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2

ifneq ($(KERNEL_HEADERS),)
CPPFLAGS += -isystem $(KERNEL_HEADERS)
endif

LDFLAGS ?=
LDLIBS ?=

SOURCES := src/main.c src/daemon.c src/mapping.c src/log.c
OBJECTS := $(SOURCES:src/%.c=$(OBJDIR)/%.o)
TARGET := ds4xbox
TEST_BINS := tests/fake_ds4 tests/read_pad

.PHONY: all clean test tests test-integration install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJDIR)/%.o: src/%.c src/common.h | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

test: $(TARGET)
	./$(TARGET) --self-test

tests: $(TEST_BINS)

tests/fake_ds4: tests/fake_ds4.c src/common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

tests/read_pad: tests/read_pad.c src/common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

test-integration: tests
	./tests/integration.sh

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TEST_BINS)
