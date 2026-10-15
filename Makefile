# VTesse build.
#   make CROSS_COMPILE=mipsel-linux-musl- STATIC=1   cross compile, static binary
#   make LWIP=0                                       TAP backend only (no stack, no proxy)
#   make LWIP_PROFILE=medium                          lwIP memory profile: large (default),
#                                                     medium (cameras, routers), small
#   make SOCKS=0 UDP_RELAY=0 TAP=0                   leave out optional modules (default 1)
#   (use a separate BUILD=... directory per variant: objects are not rebuilt on a change)
CROSS_COMPILE ?=
ifneq ($(CROSS_COMPILE),)
CC      := $(CROSS_COMPILE)gcc
AR      := $(CROSS_COMPILE)ar
endif
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -Os -g
STATIC  ?= 0
# Required flags stay separate so that CFLAGS can be overridden on the command line.
LWIP_DIR    := third_party/lwip
INCLUDES    := -Iinclude -Iport/lwip -I$(LWIP_DIR)/src/include
BASE_CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Wshadow -D_POSIX_C_SOURCE=200809L \
               -ffunction-sections -fdata-sections $(INCLUDES)
BASE_LDFLAGS := -Wl,--gc-sections
ifeq ($(STATIC),1)
BASE_LDFLAGS += -static
endif
# vendored lwIP: unmodified upstream code, built with relaxed warnings
LWIP_CFLAGS := -std=gnu99 -Wall -Wno-unused-parameter -Wno-address \
               -ffunction-sections -fdata-sections $(INCLUDES)
LDFLAGS ?=
BUILD   ?= build
LWIP    ?= 1
# lwIP memory profile (port/lwip/lwipopts.h): large (PCs, Android), medium (cameras,
# routers), small
LWIP_PROFILE ?= large
# optional modules of the lwIP build (1 = in, 0 = out): SOCKS5 server, UDP relay,
# TAP/TUN backend (kernel interface; Linux only)
SOCKS     ?= 1
UDP_RELAY ?= 1
TAP       ?= 1

NETCORE_SRC := netcore/tunnel.c netcore/loop.c netcore/route.c
ifeq ($(TAP),1)
NETCORE_SRC += netcore/tapdev.c
else
BASE_CFLAGS += -DVTESSE_WITH_TAP=0
endif
VTUN_SRC    := vtun/vtun_proto.c vtun/vtun_client.c
LWIP_SRC    := $(addprefix $(LWIP_DIR)/src/core/, def.c inet_chksum.c init.c ip.c mem.c \
                 memp.c netif.c pbuf.c stats.c sys.c tcp.c tcp_in.c tcp_out.c timeouts.c udp.c) \
               $(addprefix $(LWIP_DIR)/src/core/ipv4/, dhcp.c etharp.c icmp.c ip4.c ip4_addr.c ip4_frag.c) \
               $(LWIP_DIR)/src/netif/ethernet.c

LWIP_PROFILE_NUM := $(if $(filter small,$(LWIP_PROFILE)),0,$(if $(filter medium,$(LWIP_PROFILE)),1,2))
BASE_CFLAGS += -DVTESSE_LWIP_PROFILE=$(LWIP_PROFILE_NUM)
LWIP_CFLAGS += -DVTESSE_LWIP_PROFILE=$(LWIP_PROFILE_NUM)

ifeq ($(LWIP),1)
NETCORE_SRC += netcore/stack.c netcore/proxy.c
BASE_CFLAGS += -DNC_WITH_SOCKS=$(SOCKS)
ifeq ($(UDP_RELAY),1)
NETCORE_SRC += netcore/udprelay.c
else
BASE_CFLAGS += -DVTESSE_WITH_UDP_RELAY=0
endif
NETCORE_OBJ := $(NETCORE_SRC:%.c=$(BUILD)/%.o) $(LWIP_SRC:%.c=$(BUILD)/%.o)
else
BASE_CFLAGS += -DVTESSE_WITH_LWIP=0
NETCORE_OBJ := $(NETCORE_SRC:%.c=$(BUILD)/%.o)
endif
VTUN_OBJ    := $(VTUN_SRC:%.c=$(BUILD)/%.o)

LIBNETCORE := $(BUILD)/libnetcore.a
LIBVTESSE  := $(BUILD)/libvtesse.a
CLI        := $(BUILD)/vtesse-cli
TESTS      := $(BUILD)/test_proto $(BUILD)/test_client $(BUILD)/test_route

.PHONY: all test tests clean

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
	$(CC) $(BASE_LDFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/test_%: $(BUILD)/tests/test_%.o $(LIBVTESSE) $(LIBNETCORE)
	$(CC) $(BASE_LDFLAGS) $(LDFLAGS) -o $@ $^

tests: $(TESTS)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
