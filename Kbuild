# ===========================================================================
# Kbuild — WHAT the module is built from, not how
# ===========================================================================
#
# A Kbuild file is a fragment contributed to the kernel's own build system.
# Note what is absent: no compiler invocation, no flags, no rules.  Just
# variable assignments.  Kbuild supplies the how, because a module must be
# compiled with exactly the flags and config of the kernel it loads into —
# see the long note in kernel/Makefile for what goes wrong otherwise.
#
# Kbuild reads a file named `Kbuild` in preference to `Makefile`.  That
# preference is why this file must exist: without it, Kbuild parses whatever
# Makefile it finds as the module fragment, finds no obj-m, builds nothing,
# and exits 0.
#
# --- why this lives at the REPO ROOT rather than in kernel/ -----------------
# The module is built from two directories: kernel/ holds the VFS translation
# layer, and core/ holds the filesystem logic that is compiled twice — once
# here with kernel flags, and once by core/Makefile into libbitter.a for the
# userspace tools and tests.
#
# Kbuild builds each object into a directory beneath M=.  With M=kernel/, a
# line reading `bitterfs-y += ../core/btree.o` asks it to write
# kernel/../core/btree.o, which is outside the tree it was told to manage:
# sometimes it works, and the objects it leaves behind are ones `make clean`
# will not remove.  kernel/Kbuild flagged this as UNRESOLVED and said to
# decide it at phase 5 with the real files in hand.
#
# Decided: move M= UP to the repo root instead of reaching down from kernel/.
# Every object is then genuinely beneath M= and no path contains a `..`.
#
# The second gain is the include path.  `-I$(src)` below resolves to this
# directory, which is the same directory core/Makefile passes as `-I$(TOP)`.
# So `#include "core/btree.h"` means the identical thing in both builds, and
# a core/ source file compiles into the kernel module and into a userspace
# unit test with no #ifdef and no per-directory include juggling.  That was
# always the plan recorded in core/Makefile's "Include convention" section —
# it just assumed M= would be kernel/.
#
# The cost: kernel/ is no longer self-contained, and bitterfs.ko lands here
# rather than beside its sources.  kernel/Makefile still drives the build.
# ===========================================================================

# obj-m: build this as a loadable module (-m), producing bitterfs.ko.
# The counterpart is obj-y, which links the code into the kernel image itself.
obj-m := bitterfs.o

# One -I, pointing at the repo root, so every include is written with its
# directory visible.  See the note above; bare "btree.h" would work in one
# build and break in the other.
ccflags-y += -I$(src)

# --- The VFS layer ---------------------------------------------------------
# Listed explicitly rather than globbed, unlike core/ below.  These files
# arrive one per phase and each one is a deliberate addition, so the list
# doubles as a map of how much of the VFS surface is implemented.
#
bitterfs-y := kernel/super.o
bitterfs-y += kernel/assert.o
bitterfs-y += kernel/env_kernel.o
bitterfs-y += kernel/inode.o
bitterfs-y += kernel/dir.o
bitterfs-y += kernel/file.o
bitterfs-y += kernel/trans.o

# Where this is heading (phases 5+), one file per group of operations:
#
#   bitterfs-y += kernel/inode.o      # inode_operations — the namespace ops
#   bitterfs-y += kernel/dir.o        # directory f_ops (iterate_shared)
#   bitterfs-y += kernel/file.o       # file f_ops + address_space_operations
#   bitterfs-y += kernel/ioctl.o      # snapshot / subvolume ioctls
#   bitterfs-y += kernel/env_kernel.o # bitter_env over sb_bread/kmalloc

# --- The freestanding core -------------------------------------------------
# Globbed, deliberately, and for the same reason core/Makefile globs: core/ is
# a flat directory of independent modules, and a file forgotten from a list
# here surfaces as an "undefined reference" during modpost rather than as
# anything pointing at this line.
#
# Globbing also keeps the two builds from drifting.  A new core/ file is
# picked up by libbitter.a and by the module on the same day, so `make
# freestanding` cannot pass on a file the module never compiles.
#
# $(src) is absolute, so the wildcard needs the full path and the patsubst
# strips it back to the M=-relative form Kbuild wants.
bitterfs-y += $(patsubst $(src)/%.c,%.o,$(wildcard $(src)/core/*.c))

# --- Assertions ------------------------------------------------------------
# core/bitter_assert.h compiles BITTER_ASSERT out entirely unless BITTER_DEBUG
# is defined, and turns it into a call to bitter_assert_fail() when it is.
# That function is a LINK-time seam: core/ declares it and the consumer
# defines it — user/util.c prints and abort()s, and kernel/ needs a version
# built on BUG() or panic().
#
# ON, and kernel/assert.c is the definition — pr_emerg followed by BUG(),
# which traps on ud2 and kills the task.  These two lines are one change: the
# flag with no definition is an undefined symbol at modpost, and the definition
# with no flag is a function nothing calls.
#
# KASAN is already on in this config, and phase 5's whole difficulty is that
# the kernel does not tolerate what userspace did.  Assertions off in the
# environment that needs them most would be exactly backwards.
ccflags-y += -DBITTER_DEBUG
