# ===========================================================================
# bitterfs — core library (userspace build)
# ===========================================================================
#
#   make                 build libbitter.a
#   make freestanding    prove core/ needs no libc  <-- run this often
#   make clean           remove build products
#   make vars            show resolved paths
#
# --- what this directory is ------------------------------------------------
# core/ holds the filesystem logic that has no opinion about where it runs:
# B-tree search and insert, copy-on-write, extent accounting, checksums.  It is
# compiled TWICE from the same sources:
#
#   1. here, by this Makefile, into libbitter.a for the userspace tools and
#      tests -- where a bug is a segfault under gdb, in a second;
#   2. by kernel/Kbuild, with the kernel's own flags, into bitterfs.ko --
#      where the same bug is a guest panic.
#
# Everything gained by testing in userspace is lost the moment core/ acquires a
# dependency the kernel cannot satisfy.  That is what `make freestanding`
# defends, and why it is worth running before every commit rather than
# discovering the problem at phase 5.
# ===========================================================================

# Resolve the repo root from THIS file's location, so the paths are the same
# whether make is invoked here, from user/, or from a top-level wrapper.
# $(lastword $(MAKEFILE_LIST)) is the file currently being read.
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)

# Deliberately NOT including $(TOP)/config.mk.  That file exists to pin the
# kernel tree so kernel/ and qemu/ cannot disagree about it; nothing in this
# userspace build touches the kernel, and including it would imply otherwise.

LIB := libbitter.a

# --- Include convention ----------------------------------------------------
# One -I, pointing at the repo root, so every include is written with its
# directory visible:
#
#     #include "format/bitterfs_format.h"
#     #include "core/bitter_endian.h"
#
# This is not decoration.  kernel/Kbuild will use `ccflags-y += -I$(src)/..`,
# which resolves to the same root -- so a source file compiles identically in
# both builds with no #ifdef and no per-directory include juggling.  Bare
# "bitter_endian.h" would work here and break there.
INCLUDES := -I$(TOP)

# --- Warnings --------------------------------------------------------------
# Chosen for what they catch in THIS project rather than for completeness:
#
#   -Wpedantic            flags the GNU extensions we lean on, so each one is
#                         a deliberate choice.  (__attribute__((packed)) is
#                         exempt -- it lives in the reserved __ namespace.)
#   -Wshadow              a local named `level` hiding the node's `level` is
#                         exactly the kind of B-tree bug that reads correct.
#   -Wmissing-prototypes  a non-static function with no declaration in a header
#                         is one core/ meant to keep private.  Catches the
#                         missing `static` before it becomes a kernel symbol.
#   -Wstrict-prototypes   `f()` means "unspecified arguments" in C11, not
#                         "no arguments".  -Wold-style-definition is its half.
#   -Wvla                 a variable-length array is stack allocation the
#                         kernel's 8-16K stack cannot absorb.  Ban it here,
#                         where it is cheap, not after a stack overflow there.
#
# Not enabled: -Wconversion.  Byte-wise endian accessors are wall-to-wall
# implicit narrowing by construction, and the true positives would be invisible
# among them.
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wvla \
            -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition \
            -Wpointer-arith -Wwrite-strings

# -std=c11 explicitly, not gnu11: _Static_assert and __builtin_offsetof are
# what format/ relies on, and both are available in strict C11.  Pinning the
# standard means -Wpedantic has a fixed thing to be pedantic about.
STD := -std=c11

# Assertions on for the userspace build of core/.  The kernel build gets its
# own setting from kernel/Kbuild, so the two can differ.
DEFINES := -DBITTER_DEBUG

# CC and CFLAGS use ?= so the environment still wins:
#   make CC=clang            second opinion on warnings
#   make CFLAGS='-O0 -g3'    debugging
CC     ?= cc
CFLAGS ?= -O2 -g

# -MMD -MP writes a .d file beside each .o listing the headers it used, so
# editing format/bitterfs_format.h rebuilds everything that included it.
# Without this, a changed struct silently leaves stale objects behind -- and
# a stale object in a filesystem means a field read at the wrong offset.
# -MP adds a phony target per header so a DELETED header does not wedge make.
DEPFLAGS := -MMD -MP

ALL_CFLAGS = $(STD) $(WARNINGS) $(DEFINES) $(CFLAGS) $(DEPFLAGS) $(INCLUDES)

# --- Sources ---------------------------------------------------------------
# Discovered rather than listed: core/ is a flat directory of independent
# modules, and a forgotten line in this file would surface as an "undefined
# reference" rather than as anything pointing here.
# --- The table generator, which lives OUTSIDE core/ ------------------------
# gen_crc32c_table.c is a HOSTED program: it calls printf, so it includes
# <stdio.h> and could never compile into the kernel module.  That is why it is
# in ../tools/ rather than here.
#
# The rule core/ exists to keep is that EVERY file in this directory compiles
# freestanding -- which is exactly what `make freestanding` asserts, and the
# assertion is only worth making if it is unconditional.  Keeping the generator
# here would have meant filtering it out of SRCS and out of that check, turning
# the invariant into "everything except the file named in GEN_SRC".  The first
# exception is how exception lists start.
#
# (The kernel does put lib/gen_crc32table.c beside lib/crc32.c.  lib/ has no
# freestanding invariant to protect, and Kbuild marks the exception explicitly
# with `hostprogs :=`.  core/ makes a sharper promise and so pays more for an
# exception.)
#
# The RULE stays here even though the source does not, because the OUTPUT lives
# here: core/ owns the header, so core/ owns the recipe that produces it.
GEN_SRC := $(TOP)/tools/gen_crc32c_table.c
GEN_BIN := gen_crc32c_table
GEN_OUT := crc32c_table.h

SRCS := $(sort $(wildcard *.c))

# --- why the objects go in a subdirectory ----------------------------------
# Every .c in here is compiled TWICE, and until phase 5 both builds wrote
# btree.o to this directory.  $(TOP)/Kbuild builds the module with M=$(TOP),
# and Kbuild puts each object beside its source -- core/btree.o -- which is
# exactly where this Makefile used to put its own.
#
# The two objects are not interchangeable.  The kernel's is built -mcmodel=kernel
# and non-PIE, with KASAN instrumentation; archiving it into libbitter.a fails
# the userspace link with "relocation R_X86_64_32S ... can not be used when
# making a PIE object", which names the symptom and not the cause.  Worse, each
# build leaves an object NEWER than its source, so the other build sees it as up
# to date and never rebuilds -- the damage persists across a plain `make`.
#
# Kbuild cannot be moved off .o-beside-.c for an external module, so this build
# is the one that gives way.  $(OBJDIR) is ours; core/*.o belongs to Kbuild.
# The archive itself stays at core/libbitter.a, which is the only path user/ and
# tests/ ever name.
OBJDIR := build
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean vars help freestanding table

.DEFAULT_GOAL := all

help:
	@echo 'bitterfs core targets:'
	@echo '  make                 build $(LIB) (objects in $(OBJDIR)/)'
	@echo '  make freestanding    verify core/ compiles with no libc at all'
	@echo '  make table           regenerate $(GEN_OUT) from $(GEN_SRC)'
	@echo '  make clean           remove build products'
	@echo '  make vars            show resolved paths and sources'

vars:
	@echo 'TOP      = $(TOP)'
	@echo 'CC       = $(CC)'
	@echo 'CFLAGS   = $(ALL_CFLAGS)'
	@echo 'SOURCES  = $(if $(SRCS),$(SRCS),(none yet))'
	@echo 'HOSTCC   = $(HOSTCC)'
	@echo 'GENERATOR= $(if $(wildcard $(GEN_SRC)),../tools/gen_crc32c_table.c -> $(GEN_OUT),(not written yet: $(GEN_SRC)))'

all: $(LIB)

# --- The archive -----------------------------------------------------------
# A static library rather than loose objects, so user/ and tests/ name one
# thing and the linker takes only the members they actually reference.
#
# `ar` flags: r = insert/replace members, c = create without a warning,
# s = write an index (equivalent to ranlib).  Without s, linking against the
# archive fails to resolve symbols on some toolchains.
#
# $? rather than $^ would only add the CHANGED objects, which is faster but
# leaves deleted sources in the archive forever.  Rebuild it whole; it is a
# handful of files.
# The two cases are written as an if/else rather than as two rules, because
# make merges rules for the same target and would take the second recipe while
# warning about the first.
ifeq ($(SRCS),)

# Nothing to archive yet.  ar refuses an empty member list, so say what is
# actually going on rather than failing with "ar: no archive members".
$(LIB):
	@echo 'core/ has no .c files yet -- nothing to build.'
	@echo 'Header-only files (bitter_endian.h) need no compilation; they are'
	@echo 'compiled by whoever includes them.  This target starts producing a'
	@echo 'library once bitter_crc32c.c exists.'

else

$(LIB): $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo 'Built: $(CURDIR)/$@'

endif

# Order-only prerequisite (after the |): the directory must EXIST before the
# first compile, but its mtime changes every time a file is written into it, so
# a normal prerequisite would rebuild every object on every run.
$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(OBJDIR):
	@mkdir -p $@

# --- Regenerating the lookup table -----------------------------------------
# Deliberately NOT a prerequisite of `all`.  The kernel regenerates its table
# on every build; ours is generated once and committed, because the polynomial
# is fixed by the on-disk format and will never change without breaking every
# existing image.  Making it automatic would mean a build could silently
# rewrite a committed source file.
#
# So this is explicit: run it when you change the generator, and commit the
# result.  $(GEN_OUT) is a normal tracked source file, not a build product,
# which is why `clean` removes the binary but not the header.
#
# HOSTCC, not CC: this program runs on the machine doing the build.  They are
# the same today and would not be under a cross-compiler.
HOSTCC ?= cc

# The generator must print a complete, self-contained header to stdout:
#
#     /* this file is generated by gen_crc32c_table.c - do not edit */
#     #ifndef BITTER_CRC32C_TABLE_H
#     #define BITTER_CRC32C_TABLE_H
#     #include "format/bitterfs_format.h"
#     static const bt_u32 table[256] = { 0x00000000, ... };
#     #endif
#
# The #include matters: `make freestanding` compiles every header on its own to
# prove it is self-contained, and a bare table referring to bt_u32 would fail
# that check.
#
# Writing to a temporary and renaming means an aborted generator leaves the
# committed header untouched rather than truncated -- a truncated table is a
# silently wrong checksum, which is the worst outcome available here.
# No prerequisite on $(GEN_SRC): `table` is .PHONY and always runs, so listing
# it buys nothing -- and when the file is missing, make aborts with its own
# "No rule to make target" before the guard below can say anything useful.
table:
	@test -f $(GEN_SRC) || { echo 'ERROR: $(GEN_SRC) does not exist yet.'; \
		echo '       The generator is a hosted program and lives in tools/, not core/.'; exit 1; }
	@# $(INCLUDES) so the generator MAY include format/bitterfs_format.h and use
	@# bt_u32 if you prefer.  It does not have to -- it is a hosted program and
	@# <stdint.h> works there -- but failing on an include that resolves
	@# everywhere else in the project would be a confusing way to find out.
	$(HOSTCC) $(STD) -Wall -Wextra -O2 $(INCLUDES) -o $(GEN_BIN) $(GEN_SRC)
	./$(GEN_BIN) > $(GEN_OUT).tmp
	@test -s $(GEN_OUT).tmp || { echo 'ERROR: generator produced nothing.'; rm -f $(GEN_OUT).tmp; exit 1; }
	@mv $(GEN_OUT).tmp $(GEN_OUT)
	@rm -f $(GEN_BIN)
	@echo 'Regenerated $(CURDIR)/$(GEN_OUT) -- commit it.'

# --- The constraint check --------------------------------------------------
# The one target in this file that tests something the ordinary build cannot.
#
#   -nostdinc      removes every default include directory, exactly as the
#                  kernel's own Makefile does.  <stdio.h>, <string.h> and
#                  <stdint.h> stop existing.
#   -ffreestanding no libc, no assumption that main() exists, and no licence
#                  for the compiler to replace a loop with a call to memset.
#
# If this passes, core/ can be compiled into the kernel module.  If it fails,
# somebody reached for libc -- and the error names the file and the header, on
# the day it was introduced rather than at phase 5 under a wall of Kbuild
# output.
#
# -Werror ONLY here, and it is load-bearing.  An implicit declaration is a
# WARNING by default, so a file calling memcmp with no declaration in scope
# compiled cleanly under this check and was then rejected by the kernel build,
# which uses -Werror.  A guard that reports ok for something the real build
# refuses is worse than no guard: it is a guard you trust.  Found at phase 7,
# when the superblock's magic check moved into core/.
#
# -fsyntax-only: parse and typecheck, emit nothing.  Fast, and there is no
# object worth keeping from a build with these flags.
#
# The empty case is an if/else for the same reason as $(LIB) above: a shell
# `exit 0` inside a guard would leave the subshell, not the recipe, and the
# check would print its excuse and then run anyway.
ifeq ($(strip $(SRCS)$(wildcard *.h)),)

freestanding:
	@echo 'core/ is empty -- nothing to check.'

else

freestanding:
	@echo 'Checking core/ compiles with no standard library...'
	@for f in $(SRCS); do \
		printf '  %-28s' "$$f"; \
		$(CC) $(STD) $(WARNINGS) -Werror $(DEFINES) -nostdinc -ffreestanding $(INCLUDES) \
		      -fsyntax-only "$$f" || exit 1; \
		echo 'ok'; \
	done
	@# Headers are checked too: a header-only file like bitter_endian.h is
	@# never compiled on its own by the rule above, so a libc include hiding
	@# in one would go unnoticed until something included it.  Compiling each
	@# header as a translation unit of its own also proves it is
	@# self-contained -- that it includes what it uses instead of relying on
	@# the includer having done so first.
	@for h in $(wildcard *.h); do \
		printf '  %-28s' "$$h"; \
		echo "#include \"core/$$h\"" > .selftest.c; \
		$(CC) $(STD) $(WARNINGS) -Werror $(DEFINES) -nostdinc -ffreestanding $(INCLUDES) \
		      -fsyntax-only .selftest.c || { rm -f .selftest.c; exit 1; }; \
		echo 'ok'; \
	done
	@rm -f .selftest.c
	@echo 'core/ is freestanding.'

endif

# *.o and *.d are Kbuild's, not ours, and `make -C kernel clean` removes them.
# They are listed here anyway because they were OURS before phase 5 moved the
# build into $(OBJDIR), and a tree that has been through that change otherwise
# keeps a set of stale objects nothing ever cleans.
clean:
	rm -rf $(OBJDIR)
	rm -f $(LIB) .selftest.c $(GEN_BIN) $(GEN_OUT).tmp
	rm -f *.o *.d

# Pull in the generated header dependencies.  The leading - suppresses the
# "no such file" on a clean tree, where none exist yet.
-include $(DEPS)
