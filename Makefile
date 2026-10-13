# JournalFS Makefile
#
# Targets:
#   make              build all binaries
#   make test         run the full C test suite (unit + integration +
#                     crash injection + corruption + checkpoint + stress)
#   make crash-test   run only the crash-injection suites
#   make benchmark    run the benchmark suite (writes results/*.csv)
#   make graphs       generate results/*.svg from the CSV data
#   make asan         build with AddressSanitizer + UBSan
#   make clean        remove build artefacts
#
# Everything -- filesystem, tools, tests, benchmark and graph generator --
# is plain C11.  No Python or other runtimes are required.
#
# The core (WAL, journal, recovery, checkpointing, fsck, CLI) builds without
# any FUSE dependency.  FUSE mounting is enabled automatically when libfuse3
# is detected via pkg-config.

CC       ?= cc

CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic
OPT     := -O2
DEBUG   := -g

# Extra flags used for the sanitizer build.
CFLAGS_EXTRA  ?=
LDFLAGS_EXTRA ?=

PKG_CONFIG ?= pkg-config

# --- optional FUSE 3 support -----------------------------------------
# FUSE can come from pkg-config or from a locally extracted prefix (used when
# libfuse3-dev cannot be installed system-wide).
HAVE_FUSE := 0
FUSE_CFLAGS :=
FUSE_LIBS   :=
FUSE_PREFIX ?=

ifneq ($(shell $(PKG_CONFIG) --exists fuse3 2>/dev/null && echo yes),)
    HAVE_FUSE := 1
    FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3)
    FUSE_LIBS   := $(shell $(PKG_CONFIG) --libs fuse3)
else ifneq ($(wildcard $(FUSE_PREFIX)/usr/include/fuse3/fuse.h),)
    HAVE_FUSE := 1
    FUSE_CFLAGS := -DFUSE_USE_VERSION=31 -I$(FUSE_PREFIX)/usr/include
    FUSE_LIBS   := -L$(FUSE_PREFIX)/usr/lib/x86_64-linux-gnu -lfuse3
endif

BUILD_DIR := build

CORE_CFLAGS := $(CSTD) $(WARN) $(DEBUG) $(OPT) $(CFLAGS_EXTRA) \
               -D_POSIX_C_SOURCE=200809L -Iinclude
ifeq ($(HAVE_FUSE),1)
    CORE_CFLAGS += -DHAVE_FUSE $(FUSE_CFLAGS)
endif

# --- source files -----------------------------------------------------
CORE_SRCS := src/crc32.c src/disk.c src/journal.c src/transaction.c \
             src/filesystem.c src/recovery.c src/checkpoint.c \
             src/fsck.c src/mkfs.c

CLI_SRCS  := src/main.c
TOOL_SRCS := tools/mkfs_journalfs.c tools/fsck_journalfs.c \
             tools/journal_inspect.c

TEST_SRCS := tests/jfs_test.c
BENCH_SRCS := benchmarks/jfs_bench.c
PLOT_SRCS  := benchmarks/jfs_plot.c

# FUSE operations are only compiled into the main binary.
ifeq ($(HAVE_FUSE),1)
    CLI_SRCS += src/fuse_ops.c
endif

CORE_OBJS := $(CORE_SRCS:%.c=$(BUILD_DIR)/%.o)
CLI_OBJS  := $(CLI_SRCS:%.c=$(BUILD_DIR)/%.o)
TOOL_OBJS := $(TOOL_SRCS:%.c=$(BUILD_DIR)/%.o)

BIN_JOURNALFS := $(BUILD_DIR)/journalfs
BIN_MKFS       := $(BUILD_DIR)/mkfs_journalfs
BIN_FSCK       := $(BUILD_DIR)/fsck_journalfs
BIN_INSPECT    := $(BUILD_DIR)/journal_inspect
BIN_TEST       := $(BUILD_DIR)/jfs_test
BIN_BENCH      := $(BUILD_DIR)/jfs_bench
BIN_PLOT       := $(BUILD_DIR)/jfs_plot

.PHONY: all test crash-test benchmark graphs asan clean

all: $(BIN_JOURNALFS) $(BIN_MKFS) $(BIN_FSCK) $(BIN_INSPECT) \
     $(BIN_TEST) $(BIN_BENCH) $(BIN_PLOT)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)/src $(BUILD_DIR)/tools $(BUILD_DIR)/tests \
	          $(BUILD_DIR)/benchmarks

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

$(BIN_JOURNALFS): $(CORE_OBJS) $(CLI_OBJS)
	$(CC) $(CORE_CFLAGS) -o $@ $^ -lpthread $(FUSE_LIBS) $(LDFLAGS_EXTRA)

$(BIN_MKFS): $(BUILD_DIR)/src/mkfs.o $(BUILD_DIR)/src/crc32.o \
             $(BUILD_DIR)/src/disk.o $(BUILD_DIR)/tools/mkfs_journalfs.o
	$(CC) $(CORE_CFLAGS) -o $@ $^ -lpthread $(LDFLAGS_EXTRA)

$(BIN_FSCK): $(BUILD_DIR)/src/fsck.o $(BUILD_DIR)/src/crc32.o \
             $(BUILD_DIR)/src/disk.o $(BUILD_DIR)/src/journal.o \
             $(BUILD_DIR)/tools/fsck_journalfs.o
	$(CC) $(CORE_CFLAGS) -o $@ $^ -lpthread $(LDFLAGS_EXTRA)

$(BIN_INSPECT): $(BUILD_DIR)/src/crc32.o $(BUILD_DIR)/src/disk.o \
                $(BUILD_DIR)/src/journal.o $(BUILD_DIR)/tools/journal_inspect.o
	$(CC) $(CORE_CFLAGS) -o $@ $^ -lpthread $(LDFLAGS_EXTRA)

$(BIN_TEST): $(CORE_OBJS) $(BUILD_DIR)/tests/jfs_test.o
	$(CC) $(CORE_CFLAGS) -o $@ $^ -lpthread $(LDFLAGS_EXTRA)

$(BIN_BENCH): $(CORE_OBJS) $(BUILD_DIR)/benchmarks/jfs_bench.o
	$(CC) $(CORE_CFLAGS) -o $@ $^ -lpthread $(LDFLAGS_EXTRA)

$(BIN_PLOT): $(BUILD_DIR)/benchmarks/jfs_plot.o
	$(CC) $(CORE_CFLAGS) -o $@ $^ $(LDFLAGS_EXTRA)

# Functional shell smoke test + the full C suite.
test: all
	bash tests/test_basic.sh
	$(BIN_TEST)

crash-test: all
	$(BIN_TEST) crash random atomic

benchmark: all
	@mkdir -p results
	$(BIN_BENCH)

graphs: $(BIN_PLOT)
	@mkdir -p results
	$(BIN_PLOT)

asan:
	$(MAKE) CFLAGS_EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS_EXTRA="-fsanitize=address,undefined" \
		DEBUG="-g" OPT="-O1" all

clean:
	rm -rf $(BUILD_DIR)
	rm -f journalfs.img
	rm -f results/*.csv results/*.svg results/*.png
