# VTesse build. Cross compile with e.g.: make CC=mipsel-linux-musl-gcc
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -Os -g
# Required flags stay separate so that CFLAGS can be overridden on the command line.
BASE_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS ?=
BUILD   ?= build

NETCORE_SRC := netcore/tunnel.c netcore/loop.c
VTUN_SRC    := vtun/vtun_proto.c vtun/vtun_client.c

NETCORE_OBJ := $(NETCORE_SRC:%.c=$(BUILD)/%.o)
VTUN_OBJ    := $(VTUN_SRC:%.c=$(BUILD)/%.o)

LIBNETCORE := $(BUILD)/libnetcore.a
LIBVTESSE  := $(BUILD)/libvtesse.a
CLI        := $(BUILD)/vtesse-cli
TESTS      := $(BUILD)/test_proto $(BUILD)/test_client

.PHONY: all test clean

all: $(LIBNETCORE) $(LIBVTESSE) $(CLI)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIBNETCORE): $(NETCORE_OBJ)
	$(AR) rcs $@ $^

$(LIBVTESSE): $(VTUN_OBJ)
	$(AR) rcs $@ $^

$(CLI): $(BUILD)/cli/vtesse_cli.o $(LIBVTESSE) $(LIBNETCORE)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_%: $(BUILD)/tests/test_%.o $(LIBVTESSE) $(LIBNETCORE)
	$(CC) $(LDFLAGS) -o $@ $^

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
