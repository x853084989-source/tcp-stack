# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

# binary name
APP = myProgram
.DEFAULT_GOAL := all

# Resolve shared headers from the project root in both build systems.
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# all source are stored in SRCS-y
SRCS-y := main.c \
          protocol_worker.c \
          network_interface.c \
          transport/tcp.c \
          transport/udp.c \
          transport/tcp_socket.c \
          transport/udp_socket.c \
          network/ipv4.c \
          network/icmp.c \
          link/ethernet.c \
          link/arp.c \
          link/frame_queue.c \
          link/io_worker.c \
          dpdk/port_init.c \
          dpdk/kni.c \
          monitor/ddos.c \
          socket/event_poll.c \
          socket/socket_api.c \
          app/udp_app.c \
          app/tcp_app.c

# Build using pkg-config variables if possible
ifeq ($(shell pkg-config --exists libdpdk && echo 0),0)

all: shared
.PHONY: shared static
shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)

PKGCONF=pkg-config --define-prefix

PC_FILE := $(shell $(PKGCONF) --path libdpdk)
CFLAGS += -O3 -pthread -I$(PROJECT_ROOT) $(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += -DALLOW_EXPERIMENTAL_API
LDFLAGS_SHARED = -lm -pthread $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = -lm -pthread -Wl,-Bstatic $(shell $(PKGCONF) --static --libs libdpdk)

build/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

build:
	@mkdir -p $@

.PHONY: clean
clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared
	test -d build && rmdir -p build || true

else # Build using legacy build system

ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, detect a build directory, by looking for a path with a .config
RTE_TARGET ?= $(notdir $(abspath $(dir $(firstword $(wildcard $(RTE_SDK)/*/.config)))))

include $(RTE_SDK)/mk/rte.vars.mk

ifneq ($(CONFIG_RTE_EXEC_ENV_LINUX),y)
$(error This application can only operate in a linux environment, \
please change the definition of the RTE_TARGET environment variable)
endif

CFLAGS += -O3 -pthread -I$(PROJECT_ROOT)
CFLAGS += -DALLOW_EXPERIMENTAL_API
CFLAGS += $(WERROR_FLAGS)

LDLIBS += -lpthread -lm

include $(RTE_SDK)/mk/rte.extapp.mk
endif
