# Builds the link daemon (mcdma-rpcd), its helper library (libmcdma-rpc) and the one-sided library (libmcdma-fabric)
# into build/rpc. macOS links Apple's librdma (Thunderbolt RDMA, and MCDMA's provider); Linux links rdma-core's libibverbs.
UNAME := $(shell uname -s)
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
PREFIX ?= /usr/local
OUT ?= ../build/rpc

ifeq ($(UNAME),Darwin)
VERBS := -lrdma
HELPER := libmcdma-rpc.dylib
FABRIC := libmcdma-fabric.dylib
SHARED_HELPER := -dynamiclib -install_name $(PREFIX)/lib/$(HELPER)
SHARED_FABRIC := -dynamiclib -install_name $(PREFIX)/lib/$(FABRIC)
# Metal buffers over mailbox memory, for GPUs that read what the NIC wrote.
HELPER_EXTRA := libmcdma_rpc_metal.m -fobjc-arc -framework Metal -framework Foundation
GPU_TOOLS := $(OUT)/metal-poll
else
# shm_open lives in librt before glibc 2.34; dlsym finds ibv_reg_dmabuf_mr where libibverbs has it
VERBS := -libverbs -lrt
HELPER := libmcdma-rpc.so
FABRIC := libmcdma-fabric.so
SHARED_HELPER := -shared -fPIC
SHARED_FABRIC := -shared -fPIC
FABRIC_EXTRA := -ldl
endif

LINK := link_verbs.c link_tb.c link_xchg.c
DAEMON := mcdma-rpcd.c rpcd_common.c rpcd_verbs.c rpcd_listen.c rpcd_connect.c $(LINK)
# A checksum of the fabric's sources, which fabric-check prints so a qualification can refuse two different builds.
BUILD_ID := $(shell cat libmcdma_fabric.c fabric_bond_n.h fabric_open_qps.h fabric_trace.h fabric_trace_report.h mcdma_fabric.h $(LINK) link.h fabric_check.c | cksum | cut -d' ' -f1)

all: $(OUT)/mcdma-rpcd $(OUT)/$(HELPER) $(OUT)/$(FABRIC) $(OUT)/rpc-echo $(OUT)/fabric-check $(OUT)/mesh-check \
     $(GPU_TOOLS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/mcdma-rpcd: $(DAEMON) rpcd.h link.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(DAEMON) $(VERBS) -lpthread

$(OUT)/$(HELPER): libmcdma_rpc.c mcdma_rpc.h | $(OUT)
	$(CC) $(CFLAGS) $(SHARED_HELPER) -o $@ libmcdma_rpc.c $(HELPER_EXTRA)

# Only the mcdma_fabric_* functions are exported; the shared link code stays private to the library.
$(OUT)/$(FABRIC): libmcdma_fabric.c fabric_bond_n.h fabric_open_qps.h fabric_trace.h fabric_trace_report.h mcdma_fabric.h $(LINK) link.h | $(OUT)
	$(CC) $(CFLAGS) $(SHARED_FABRIC) -fvisibility=hidden -o $@ libmcdma_fabric.c $(LINK) $(VERBS) -lpthread $(FABRIC_EXTRA)

# Hardware qualification tools: an echo service and client for a daemon link, and a two-rank check of the fabric.
$(OUT)/rpc-echo: rpc_echo.c rpcd_common.c rpcd.h link.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ rpc_echo.c rpcd_common.c link_verbs.c link_tb.c $(VERBS) -lpthread

# fabric-check carries the library's code itself, so it runs from the build directory with nothing installed.
$(OUT)/fabric-check: fabric_check.c libmcdma_fabric.c fabric_bond_n.h fabric_open_qps.h fabric_trace.h fabric_trace_report.h mcdma_fabric.h $(LINK) link.h | $(OUT)
	$(CC) $(CFLAGS) -DMCDMA_BUILD_ID='"$(BUILD_ID)"' -o $@ fabric_check.c libmcdma_fabric.c $(LINK) $(VERBS) -lpthread \
		$(FABRIC_EXTRA)

# Four-node mesh qualification, and on a Mac the GPU hand-off check (docs/fabric.md).
$(OUT)/mesh-check: mesh_check.c libmcdma_fabric.c fabric_bond_n.h fabric_open_qps.h fabric_trace.h fabric_trace_report.h mcdma_fabric.h $(LINK) link.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ mesh_check.c libmcdma_fabric.c $(LINK) $(VERBS) -lpthread $(FABRIC_EXTRA)

$(OUT)/metal-poll: metal_poll.m $(LINK) link.h | $(OUT)
	$(CC) $(CFLAGS) -fobjc-arc -o $@ metal_poll.m $(LINK) -framework Metal -framework Foundation $(VERBS) -lpthread

test: ../tests/test_rpc_helper.c ../tests/test_rpcd_socket.c ../tests/test_rpcd_hosts.c libmcdma_rpc.c mcdma_rpc.h \
      rpcd_common.c rpcd.h | $(OUT)
	$(CC) $(CFLAGS) -o $(OUT)/test-rpc-helper ../tests/test_rpc_helper.c libmcdma_rpc.c $(HELPER_EXTRA) -lpthread
	$(OUT)/test-rpc-helper
	$(CC) $(CFLAGS) -o $(OUT)/test-rpcd-socket ../tests/test_rpcd_socket.c rpcd_common.c
	$(OUT)/test-rpcd-socket
	$(CC) $(CFLAGS) -o $(OUT)/test-rpcd-hosts ../tests/test_rpcd_hosts.c rpcd_common.c
	$(OUT)/test-rpcd-hosts
	cd .. && python3 -B tests/test_rpcd_daemon.py
	cd .. && python3 -B tests/test_link.py
	$(MAKE) -C gpu test

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 0755 $(OUT)/mcdma-rpcd $(DESTDIR)$(PREFIX)/bin/
	install -m 0644 $(OUT)/$(HELPER) $(OUT)/$(FABRIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 mcdma_rpc.h mcdma_fabric.h $(DESTDIR)$(PREFIX)/include/

clean:
	rm -rf $(OUT)

.PHONY: all test install clean
