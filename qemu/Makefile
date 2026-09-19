# ===========================================================================
# bitterfs — guest kernel build
# ===========================================================================
#
# Reproducibly turns a pristine kernel source tree into the debuggable guest
# kernel bitterfs is developed against.  The tree itself deliberately lives
# OUTSIDE this repo (source is ~1.5G, a build adds ~20G) — this Makefile only
# knows its path.
#
#   make config     regenerate the tree's .config from scratch
#   make check      verify the options that matter actually survived
#   make kernel     build bzImage + the gdb helper scripts
#   make all        config + kernel + check, in order
#
# Override the tree location per-invocation or in your environment:
#   make KERNEL=$HOME/kernels/linux-6.6 config
# ===========================================================================

# KERNEL, JOBS and BUSYBOX live in ../config.mk so that kernel/Makefile builds
# the module against exactly the tree this Makefile boots.  $(lastword
# $(MAKEFILE_LIST)) is the file currently being read, so the include resolves
# relative to THIS Makefile rather than to whatever directory make was run from.
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
include $(TOP)/config.mk

FRAGMENT := $(CURDIR)/kernel.config
NPROC    := $(shell nproc)

BZIMAGE  := $(KERNEL)/arch/x86/boot/bzImage
VMLINUX  := $(KERNEL)/vmlinux
DOTCONFIG:= $(KERNEL)/.config

# --- initramfs -------------------------------------------------------------
# The guest root filesystem: a gzipped cpio the kernel unpacks into a tmpfs and
# runs /init from.  Deliberately passed with -initrd rather than embedded via
# CONFIG_INITRAMFS_SOURCE, so changing /init costs a ~200ms repack instead of a
# kernel relink.
#
# gen_init_cpio comes from the kernel tree and is the reason none of this needs
# root: it synthesises device nodes, ownership and modes INSIDE the archive,
# with no privilege on the host.  It is built as a side effect of `make kernel`.
# (BUSYBOX is defined in ../config.mk.)
INIT         := $(CURDIR)/init
MKINITRAMFS  := $(CURDIR)/mkinitramfs.sh
INITRAMFS    := $(CURDIR)/initramfs.cpio.gz
GEN_INIT_CPIO:= $(KERNEL)/usr/gen_init_cpio

# Options whose absence would either break the dev loop outright or silently
# remove a debugging capability you would not notice missing until you needed
# it at 2am.  `make check` is the tripwire.
REQUIRE_Y := DEBUG_KERNEL GDB_SCRIPTS KALLSYMS_ALL \
             KASAN KASAN_INLINE \
             PROVE_LOCKING DEBUG_ATOMIC_SLEEP DEBUG_LIST \
             MODULES MODULE_UNLOAD \
             VIRTIO_BLK NET_9P_VIRTIO 9P_FS SERIAL_8250_CONSOLE \
             DEVTMPFS_MOUNT

REQUIRE_N := RANDOMIZE_BASE DEBUG_INFO_REDUCED

.DEFAULT_GOAL := help
.PHONY: help all config check kernel initramfs vars clean distclean-kernel

# `all` chains steps that genuinely depend on each other (gen_init_cpio does not
# exist until the kernel is built), and make would happily run them in parallel
# under -j.  The kernel sub-make gets its own explicit -j$(JOBS), so serialising
# here costs nothing.
.NOTPARALLEL:

help:
	@echo 'bitterfs guest kernel targets:'
	@echo '  make config     regenerate $(KERNEL)/.config from defconfig + fragment'
	@echo '  make check      verify required CONFIG options survived olddefconfig'
	@echo '  make kernel     build bzImage and gdb scripts (uses -j$(JOBS))'
	@echo '  make initramfs  repack the guest root filesystem'
	@echo '  make all        config, kernel, check, initramfs'
	@echo '  make clean      remove the initramfs (leaves the kernel build alone)'
	@echo '  make vars       show resolved paths'
	@echo ''
	@echo 'Kernel tree: $(KERNEL)'

vars:
	@echo 'KERNEL    = $(KERNEL)'
	@echo 'FRAGMENT  = $(FRAGMENT)'
	@echo 'BZIMAGE   = $(BZIMAGE)'
	@echo 'VMLINUX   = $(VMLINUX)'
	@echo 'BUSYBOX   = $(BUSYBOX)'
	@echo 'INITRAMFS = $(INITRAMFS)'
	@echo 'NPROC     = $(NPROC)'
	@echo 'JOBS      = $(JOBS)'

all: config kernel check initramfs

# --- Guard -----------------------------------------------------------------
# Fail with a useful message rather than a wall of "No rule to make target".
$(KERNEL)/Makefile:
	@echo 'ERROR: no kernel source tree at $(KERNEL)'
	@echo ''
	@echo 'Fetch it with:'
	@echo '  mkdir -p $(dir $(KERNEL)) && cd $(dir $(KERNEL)) \'
	@echo '    && curl -O https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.101.tar.xz \'
	@echo '    && tar xf linux-6.12.101.tar.xz && ln -sfn linux-6.12.101 linux-6.12'
	@echo ''
	@echo 'Or point this Makefile elsewhere:  make KERNEL=/path/to/tree config'
	@false

# --- Config ----------------------------------------------------------------
# Three layers, applied in order (see kernel.config's header for why):
#   defconfig        minimal bootable x86-64 kernel
#   kvm_guest.config the tree's own fragment for running under KVM/QEMU
#   kernel.config    our debugging deltas
#
# merge_config.sh -m merges without invoking a config target, so we run
# olddefconfig ourselves afterwards to resolve every dependency the merge
# implied.  It also prints a warning for any value it overrode, which is worth
# reading — a warning here usually means an upstream default moved.
config: $(KERNEL)/Makefile
	$(MAKE) -C $(KERNEL) defconfig
	$(MAKE) -C $(KERNEL) kvm_guest.config
	cd $(KERNEL) && ./scripts/kconfig/merge_config.sh -m -O $(KERNEL) \
		$(DOTCONFIG) $(FRAGMENT)
	$(MAKE) -C $(KERNEL) olddefconfig
	@echo ''
	@echo 'Config written to $(DOTCONFIG) — now run: make check'

# --- Verify ----------------------------------------------------------------
# olddefconfig will silently drop an option whose dependencies are unmet, which
# is the failure mode this target exists to catch.  A dropped option does not
# announce itself; it just means a capability quietly is not there.
check:
	@test -f $(DOTCONFIG) || { echo 'ERROR: no $(DOTCONFIG) — run: make config'; exit 1; }
	@fail=0; \
	for sym in $(REQUIRE_Y); do \
		if grep -q "^CONFIG_$$sym=y" $(DOTCONFIG); then \
			printf '  ok    CONFIG_%s=y\n' "$$sym"; \
		else \
			printf '  FAIL  CONFIG_%s expected =y\n' "$$sym"; \
			fail=1; \
		fi; \
	done; \
	for sym in $(REQUIRE_N); do \
		if grep -q "^CONFIG_$$sym=y" $(DOTCONFIG); then \
			printf '  FAIL  CONFIG_%s expected unset\n' "$$sym"; \
			fail=1; \
		else \
			printf '  ok    CONFIG_%s unset\n' "$$sym"; \
		fi; \
	done; \
	echo ''; \
	if [ $$fail -ne 0 ]; then \
		echo 'Some required options did not survive olddefconfig.'; \
		echo 'Usually an unmet dependency — inspect with:'; \
		echo '  make -C $(KERNEL) menuconfig   (press / to search a symbol)'; \
		exit 1; \
	else \
		echo 'All required options present.'; \
	fi

# --- Build -----------------------------------------------------------------
# bzImage pulls in vmlinux as a dependency, and scripts_gdb then emits
# vmlinux-gdb.py beside it — that is what provides `lx-symbols`, the command
# that teaches gdb where a freshly-insmod'd bitterfs.ko lives in guest memory.
#
# The `modules` target is NOT redundant here, despite nothing in the guest
# config being =m.  It also produces Module.symvers — the table of every symbol
# the kernel exports, with its CRC.  An external module build (kernel/Makefile)
# reads that table to resolve symbols, and without it modpost fails on things
# you never referenced yourself:
#
#   ERROR: modpost: "__x86_return_thunk" [bitterfs.ko] undefined!
#
# Zero modules get built; the symbol table still has to be generated.  It costs
# seconds, since the compile work is already done.
kernel: $(KERNEL)/Makefile
	@test -f $(DOTCONFIG) || { echo 'ERROR: no $(DOTCONFIG) — run: make config'; exit 1; }
	$(MAKE) -C $(KERNEL) -j$(JOBS) bzImage
	$(MAKE) -C $(KERNEL) -j$(JOBS) modules
	$(MAKE) -C $(KERNEL) scripts_gdb
	@echo ''
	@echo 'Built: $(BZIMAGE)'
	@echo 'Debug: $(VMLINUX)  (gdb $(VMLINUX); target remote :1234; lx-symbols)'

# --- Initramfs -------------------------------------------------------------
# Contract with mkinitramfs.sh (which you write):
#
#   environment:  KERNEL   kernel tree — the script finds usr/gen_init_cpio in it
#                 BUSYBOX  path to a STATIC busybox binary to embed
#                 INIT     path to the /init script to embed as PID 1
#   argv[1]:               output path for the gzipped cpio
#
# Keeping the paths in the environment rather than hardcoded in the script means
# you can point the whole build at a second kernel tree or a self-built busybox
# without editing anything.
initramfs: $(INITRAMFS)

# gen_init_cpio is an ORDER-ONLY prerequisite (after the |).  It must exist, but
# its timestamp is ignored — otherwise every kernel rebuild would touch it and
# force a pointless initramfs repack.
$(INITRAMFS): $(INIT) $(MKINITRAMFS) | $(GEN_INIT_CPIO)
	@test -x $(MKINITRAMFS) || { echo 'ERROR: $(MKINITRAMFS) is not executable — chmod +x it'; exit 1; }
	@test -f $(BUSYBOX)     || { echo 'ERROR: no busybox at $(BUSYBOX) — override with make BUSYBOX=/path'; exit 1; }
	@# A dynamically linked busybox produces an initramfs that panics on boot
	@# with nothing useful on the console: the loader is missing, so PID 1 dies
	@# before it can print anything.  ldd exits non-zero on a static binary,
	@# so a *successful* ldd here is the failure case.  Catch it now rather
	@# than at boot.
	@if ldd $(BUSYBOX) >/dev/null 2>&1; then \
		echo 'ERROR: $(BUSYBOX) is dynamically linked.'; \
		echo '       An initramfs has no shared libraries, so PID 1 would die'; \
		echo '       before printing anything. Install busybox-static, or build'; \
		echo '       busybox with CONFIG_STATIC=y and pass BUSYBOX=<path>.'; \
		exit 1; \
	fi
	KERNEL=$(KERNEL) BUSYBOX=$(BUSYBOX) INIT=$(INIT) $(MKINITRAMFS) $@
	@echo ''
	@echo "Packed: $@ ($$(du -h $@ | cut -f1))"

# Guards, matching the kernel-tree one above: fail with the fix rather than
# "No rule to make target".  These are the two files you write by hand.
$(INIT) $(MKINITRAMFS):
	@echo 'ERROR: $@ does not exist yet.'
	@echo ''
	@echo 'The initramfs needs two hand-written files in qemu/:'
	@echo '  init            PID 1 — mounts /proc /sys /dev and the 9p share,'
	@echo '                  then execs /mnt/src/qemu/guest.sh or a shell'
	@echo '  mkinitramfs.sh  emits a gen_init_cpio spec (static entries plus one'
	@echo '                  slink per `busybox --list` applet) and packs it'
	@false

$(GEN_INIT_CPIO):
	@echo 'ERROR: $(GEN_INIT_CPIO) does not exist.'
	@echo ''
	@echo 'It is built as part of the kernel build. Run:'
	@echo '  make kernel'
	@false

clean:
	rm -f $(INITRAMFS)

# --- Nuclear option --------------------------------------------------------
# Separate, explicitly named, and never a dependency of anything: this throws
# away a 20-45 minute build.  Reach for it only when a config change has left
# the tree in an inconsistent state that olddefconfig cannot untangle.
distclean-kernel: $(KERNEL)/Makefile
	@printf 'This deletes the entire build in %s. Type YES to confirm: ' '$(KERNEL)'
	@read ans; test "$$ans" = YES || { echo 'aborted'; exit 1; }
	$(MAKE) -C $(KERNEL) distclean
