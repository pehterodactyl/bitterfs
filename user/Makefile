# ===========================================================================
# bitterfs — userspace tools
# ===========================================================================
#
#   make                    build every tool whose source exists
#   make mkfs.bitterfs      build one
#   make SANITIZE=1         rebuild with ASan + UBSan (see below)
#   make clean              remove build products
#   make vars               show what will be built and what is being skipped
#
# --- what lives here -------------------------------------------------------
#   mkfs.bitterfs   writes an empty filesystem: superblock, an empty root
#                   tree, an empty FS tree.  The first thing that produces an
#                   image at all.
#   bitter-dump     reads an image back and prints its trees.  The debugger
#                   for every phase after this one -- when a mount fails at
#                   phase 5, this is what tells you whether the image was
#                   already wrong before the kernel touched it.
#   bitter-fsck     phase 8.  Listed now so the pattern is visible.
#
# These link against ../core/libbitter.a, so the checksum and B-tree code the
# tools exercise is byte-for-byte the code the kernel module runs.  A tool with
# its own private copy of crc32c would agree with itself and disagree with the
# filesystem, which is the worst possible outcome for a verification tool.
# ===========================================================================

TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)

CORE := $(TOP)/core
LIB  := $(CORE)/libbitter.a

# Same include convention as core/Makefile: one -I at the repo root, so every
# include names its directory and the file compiles the same way everywhere.
INCLUDES := -I$(TOP)

# --- Warnings --------------------------------------------------------------
# Deliberately NOT identical to core/'s set.  Two differences, both intended:
#
#   -Wmissing-prototypes is dropped.  These are programs, not a library; main()
#   has no prototype and does not want one.
#
#   -Wformat=2 is added.  Only userspace has printf, and bitter-dump is almost
#   entirely printf.  It catches a %lu given a bt_u64 -- which is correct on
#   x86-64 and wrong on 32-bit, i.e. exactly the bug that survives testing.
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wvla \
            -Wstrict-prototypes -Wold-style-definition \
            -Wpointer-arith -Wwrite-strings -Wformat=2

STD := -std=c11

# _FILE_OFFSET_BITS=64 makes off_t 64-bit on 32-bit hosts, so lseek, pread and
# stat work past 2GB.  A no-op on x86-64 and free insurance elsewhere: without
# it, the first image larger than 2GB fails with EOVERFLOW from a call whose
# arguments all look correct.
#
# _DEFAULT_SOURCE exposes what strict C11 hides from glibc, notably pread and
# pwrite -- which -std=c11 (rather than -std=gnu11) would otherwise leave
# undeclared.  Keeping strict C11 and asking for the POSIX pieces explicitly
# means the file states which extensions it depends on.
DEFINES := -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE -DBITTER_DEBUG

CC     ?= cc
CFLAGS ?= -O2 -g

DEPFLAGS := -MMD -MP

# --- SANITIZE=1 ------------------------------------------------------------
# AddressSanitizer and UndefinedBehaviorSanitizer, roughly 2x slower and worth
# it for anything that touches a buffer by computed offset -- which is all of
# this.  An off-by-one writing an item into a leaf becomes an immediate report
# naming the line, rather than a corrupt image discovered three phases later.
#
# UBSan also catches the misaligned accesses that packed structures invite, and
# the shift-past-width mistakes the endian accessors invite.
#
# Objects built with and without sanitizers are NOT interchangeable: mixing
# them produces link errors or, worse, a partially instrumented binary.  Run
# `make clean` when toggling this.
ifeq ($(SANITIZE),1)
  SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
  # Halt on the first UBSan finding instead of printing and continuing.  A
  # program that has already executed undefined behaviour is not worth
  # observing further.
  SANFLAGS += -fno-sanitize-recover=undefined
endif

ALL_CFLAGS = $(STD) $(WARNINGS) $(DEFINES) $(CFLAGS) $(SANFLAGS) \
             $(DEPFLAGS) $(INCLUDES)
LDFLAGS   += $(SANFLAGS)

# --- Programs --------------------------------------------------------------
# The tool names do not match their source names -- mkfs.bitterfs is the name
# mount(8) and mkfs(8) look for, bitter-dump follows the git-style prefix
# convention -- so the mapping is written out rather than inferred.
PROGRAMS := mkfs.bitterfs bitter-dump bitter-fsck

mkfs.bitterfs_SRCS := mkfs.c
bitter-dump_SRCS   := dump.c
bitter-fsck_SRCS   := fsck.c

# Shared by all three tools: die/die_errno and the progname they print.  Not in
# core/ -- die calls exit(), which is right for a program and wrong for
# anything the kernel module links, where every failure has to be a return
# value a caller can propagate.
#
# trans.c joins them at phase 3.  It is here rather than beside one tool
# because the transaction lifecycle belongs to whoever owns the superblock, and
# core/ deliberately does not -- see docs/skeleton.tex.  tests/ links this same
# object out of SUPPORT, which is why it must be built here even while no tool
# in this directory calls it yet.
COMMON_SRCS := util.c print.c env_user.c trans.c format.c
COMMON_OBJS := $(patsubst %.c,%.o,$(wildcard $(COMMON_SRCS)))

# Build only the tools whose sources actually exist.  During phase 1 that is
# mkfs alone, then mkfs and dump; fsck arrives at phase 8.  Without this, a
# bare `make` fails with "No rule to make target 'fsck.c'" and stops before
# building the two tools that were ready.
BUILDABLE := $(strip $(foreach p,$(PROGRAMS),\
               $(if $(wildcard $($(p)_SRCS)),$(p))))
SKIPPED   := $(filter-out $(BUILDABLE),$(PROGRAMS))

OBJS := $(sort $(COMMON_OBJS) $(foreach p,$(BUILDABLE),$($(p)_SRCS:.c=.o)))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean vars help core

.DEFAULT_GOAL := all

help:
	@echo 'bitterfs userspace tool targets:'
	@echo '  make                 build: $(if $(BUILDABLE),$(BUILDABLE),(nothing yet))'
	@echo '  make SANITIZE=1      rebuild with ASan + UBSan (make clean first)'
	@echo '  make clean           remove build products'
	@echo '  make vars            show resolved paths'

vars:
	@echo 'TOP       = $(TOP)'
	@echo 'LIB       = $(LIB)'
	@echo 'CFLAGS    = $(ALL_CFLAGS)'
	@echo 'COMMON    = $(if $(COMMON_OBJS),$(COMMON_OBJS),(util.c not written yet))'
	@echo 'BUILDABLE = $(if $(BUILDABLE),$(BUILDABLE),(none))'
	@echo 'SKIPPED   = $(if $(SKIPPED),$(SKIPPED) (source not written yet),(none))'

all: $(BUILDABLE)
	@$(if $(SKIPPED),echo 'Not built yet (no source): $(SKIPPED)',:)
	@$(if $(BUILDABLE),echo 'Built: $(BUILDABLE)',echo 'user/ has no tool sources yet -- write mkfs.c first.')

# --- Linking against core --------------------------------------------------
# The archive is a real prerequisite, not a one-off setup step: touching
# core/bitter_crc32c.c must relink these tools.  Marking `core` .PHONY hands
# the up-to-date decision to core/Makefile, which is the only place that knows
# what core depends on.
#
# MAKE rather than make, so -n, -j and the rest propagate into the sub-build.
core:
	@$(MAKE) --no-print-directory -C $(CORE)

$(LIB): core
	@:

# One recipe for every program, via a static pattern rule.  Per-program
# prerequisites are declared separately below and accumulate onto it.
#
# $(filter %.o,$^) rather than $^ because $^ includes $(LIB); listing the
# archive twice is harmless but noisy, and being explicit about object-order
# matters to the linker -- objects must precede the archive that satisfies
# them, since a static library is scanned once, left to right.
$(PROGRAMS): %: $(COMMON_OBJS) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $(filter %.o,$^) $(LIB) $(LDFLAGS)

mkfs.bitterfs: mkfs.o
bitter-dump:   dump.o
bitter-fsck:   fsck.o

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

clean:
	rm -f $(PROGRAMS) *.o *.d

-include $(DEPS)
