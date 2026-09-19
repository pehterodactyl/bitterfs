# ===========================================================================
# bitterfs — kernel module build (wrapper)
# ===========================================================================
#
#   make            build bitterfs.ko
#   make clean      remove build products
#   make info       show the .ko's vermagic and metadata
#   make vars       show resolved paths
#
# This file builds NOTHING itself.  Every target hands off to the kernel's own
# build system with `-C $(KERNEL) M=$(CURDIR)`, which reads ./Kbuild for the
# object lists and supplies all the compiler flags.
#
# --- why the flags are not ours to choose ----------------------------------
# A module is not an independent program; it links into a running kernel and
# shares its data structures directly, so it must be compiled with exactly the
# same flags and config as the kernel it loads into.
#
# Concretely, with this project's config: CONFIG_PROVE_LOCKING adds fields to
# struct mutex, and CONFIG_KASAN changes how memory accesses are emitted.  A
# module built without them computes different offsets for the same struct
# fields.  That is not a link error — it is a module that loads successfully
# and then writes to the wrong memory.  Silent corruption, surfacing anywhere
# except where it originates.
#
# Kbuild makes that impossible by construction: build against the configured
# tree, reuse its flags verbatim.  The kernel also stamps a "vermagic" string
# into every .ko (version, key config, compiler) and insmod refuses on
# mismatch — see `make info`.
# ===========================================================================

# KERNEL comes from ../config.mk, shared with qemu/Makefile so the module is
# always built against the same tree the VM boots.  $(lastword $(MAKEFILE_LIST))
# is the file being read, so this resolves relative to THIS Makefile rather than
# to wherever make was invoked from.
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
include $(TOP)/config.mk

# The module now lands at the repo root rather than beside these sources,
# because M= is $(TOP) — see the long note in $(TOP)/Kbuild.  Split into a bare
# name and a full path: the name is what the guest insmods over the 9p share,
# the path is what this Makefile tests for and hands to modinfo.
MODNAME := bitterfs.ko
MODULE  := $(TOP)/$(MODNAME)

.PHONY: all modules clean info vars help

all: modules

help:
	@echo 'bitterfs kernel module targets:'
	@echo '  make            build $(MODNAME)'
	@echo '  make clean      remove build products'
	@echo '  make info       show vermagic and module metadata'
	@echo '  make vars       show resolved paths'
	@echo ''
	@echo 'Kernel tree: $(KERNEL)'
	@echo ''
	@echo 'Load it in the guest (the .ko is visible over the 9p share):'
	@echo '  insmod /mnt/src/$(MODNAME)'
	@echo '  dmesg | tail'
	@echo '  rmmod bitterfs'

vars:
	@echo 'TOP    = $(TOP)'
	@echo 'KERNEL = $(KERNEL)'
	@echo 'MODULE = $(MODULE)'

# --- Guards ----------------------------------------------------------------
# An external module needs a tree that is both CONFIGURED and BUILT: Kbuild
# reads .config for the flags, and needs the built tree for generated headers
# and Module.symvers.  Checking here turns a confusing wall of missing-header
# errors into one line naming the fix.
$(KERNEL)/.config:
	@echo 'ERROR: $(KERNEL) is not configured.'
	@echo '  make -C $(TOP)/qemu config'
	@false

$(KERNEL)/vmlinux:
	@echo 'ERROR: $(KERNEL) is configured but not built.'
	@echo '  make -C $(TOP)/qemu kernel'
	@false

# --- Build -----------------------------------------------------------------
# -C changes into the kernel tree so ITS Makefile drives everything; M= tells it
# "there is an external module over here."  It reads ./Kbuild, compiles with its
# own flags, and writes bitterfs.ko next to these sources — which is also where
# the guest sees it, since the repo is the 9p share.  No copying, no image
# rebuild: that is the entire edit-compile-insmod loop.
modules: $(TOP)/Kbuild $(KERNEL)/.config $(KERNEL)/vmlinux
	$(MAKE) -C $(KERNEL) M=$(TOP) modules
	@# Kbuild exits 0 whether or not it produced anything, so verify rather
	@# than announce.  Without this check a build that quietly produced no
	@# module still prints a success message — which is exactly what happened
	@# the first time this Makefile ran.
	@test -f $(MODULE) || { \
		echo ''; \
		echo 'ERROR: the build succeeded but $(MODULE) was not produced.'; \
		echo '       Check that $(TOP)/Kbuild names the right objects and'; \
		echo '       that every .c file it lists exists.'; \
		exit 1; \
	}
	@echo ''
	@echo 'Built: $(MODULE)'
	@echo 'Guest: insmod /mnt/src/$(MODNAME)'

# Kbuild looks for a file named `Kbuild` first and falls back to `Makefile`.
# With no Kbuild at $(TOP), it parses whatever Makefile it finds there as the
# module fragment, finds no obj-m, builds nothing, and exits 0 — a silent no-op
# that looks like success.  Guard against that rather than debug it later.
$(TOP)/Kbuild:
	@echo 'ERROR: no Kbuild at $(TOP).'
	@echo ''
	@echo 'Without it the kernel build system parses whatever Makefile it finds'
	@echo 'there as the module fragment, finds no obj-m, and builds nothing.'
	@echo ''
	@echo 'Create $(TOP)/Kbuild containing at least:'
	@echo '    obj-m      := bitterfs.o'
	@echo '    ccflags-y  += -I$$(src)'
	@echo '    bitterfs-y := kernel/hello.o'
	@false

clean:
	$(MAKE) -C $(KERNEL) M=$(TOP) clean

# --- Diagnostics -----------------------------------------------------------
# `insmod: Invalid module format` almost always means the .ko was built against
# a different kernel than the one running.  vermagic is the string the kernel
# compares; print it here and compare against the guest's `uname -r` and the
# tree above.  Faster than guessing.
info: $(MODULE)
	@modinfo $(MODULE) | sed -n '/^filename/p;/^description/p;/^license/p;/^vermagic/p'

$(MODULE):
	@echo 'ERROR: $(MODNAME) not built yet — run: make'
	@false
