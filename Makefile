# VTesse build. Cross compile with e.g.: make CC=mipsel-linux-musl-gcc
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -Os -g
# Required flags stay separate so that CFLAGS can be overridden on the command line.
LWIP_DIR    := third_party/lwip
INCLUDES    := -Iinclude -Iport/lwip -I$(LWIP_DIR)/src/include
BASE_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -D_POSIX_C_SOURCE=200809L $(INCLUDES)
# vendored lwIP: unmodified upstream code, built with relaxed warnings
LWIP_CFLAGS := -std=gnu99 -Wall -Wno-unused-parameter -Wno-address $(INCLUDES)
LDFLAGS ?=
BUILD   ?= build

NETCORE_SRC := netcore/tunnel.c netcore/loop.c netcore/route.c netcore/stack.c
VTUN_SRC    := vtun/vtun_proto.c vtun/vtun_client.c
LWIP_SRC    := $(addprefix $(LWIP_DIR)/src/core/, def.c inet_chksum.c init.c ip.c mem.c \
                 memp.c netif.c pbuf.c stats.c sys.c tcp.c tcp_in.c tcp_out.c timeouts.c udp.c) \
               $(addprefix $(LWIP_DIR)/src/core/ipv4/, dhcp.c etharp.c icmp.c ip4.c ip4_addr.c ip4_frag.c) \
               $(LWIP_DIR)/src/netif/ethernet.c

NETCORE_OBJ := $(NETCORE_SRC:%.c=$(BUILD)/%.o) $(LWIP_SRC:%.c=$(BUILD)/%.o)
VTUN_OBJ    := $(VTUN_SRC:%.c=$(BUILD)/%.o)

LIBNETCORE := $(BUILD)/libnetcore.a
LIBVTESSE  := $(BUILD)/libvtesse.a
CLI        := $(BUILD)/vtesse-cli
TESTS      := $(BUILD)/test_proto $(BUILD)/test_client $(BUILD)/test_route

.PHONY: all test clean

all: $(LIBNETCORE) $(LIBVTESSE) $(CLI)

$(BUILD)/$(LWIP_DIR)/%.o: $(LWIP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

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
