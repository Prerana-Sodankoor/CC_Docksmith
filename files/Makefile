# Docksmith — Makefile
#
# Usage:
#   make          → build the docksmith binary
#   make clean    → remove build artefacts
#   make install  → copy binary to /usr/local/bin

CC      = gcc
CFLAGS  = -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation -I.
LDFLAGS = -lssl -lcrypto

SRCS = main.c \
       cli/cli.c \
       cli/parser.c \
       engine/engine.c \
       runtime/runtime.c

# When cache/cache.c exists, add it automatically
ifneq ($(wildcard cache/cache.c),)
    SRCS += cache/cache.c
endif

TARGET = docksmith

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@echo "Build successful: ./$(TARGET)"

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Installed to /usr/local/bin/$(TARGET)"

.PHONY: all clean install
