# ===========================================================================
# config.mk — shared settings for every bitterfs Makefile
# ===========================================================================
#
# Included by qemu/Makefile and kernel/Makefile so that the kernel tree path
# has exactly ONE definition.  This is not tidiness: a module must be built
# against precisely the tree it will be loaded into, and two copies of this
# path drifting apart produces `insmod: Invalid module format` — an error whose
# message says nothing about the actual cause.
#
# Every value uses ?= so the environment or a command-line override still wins:
#   make KERNEL=$HOME/kernels/linux-6.6 ...
# ===========================================================================

# Pinned kernel tree.  VFS method signatures churn between releases (see
# Documentation/filesystems/porting.rst) — changing this mid-project means
# revisiting every method signature in kernel/.
KERNEL  ?= $(HOME)/kernels/linux-6.12/linux-6.12.101

# Build parallelism.  A KASAN + full-DWARF kernel build is MEMORY bound rather
# than CPU bound: each parallel gcc can hold well over 1GB, and `LD vmlinux.o`
# wants several GB by itself.  Exceed available RAM and the OOM killer takes the
# linker, which surfaces as a bare "Killed" and Error 137 rather than as
# anything resembling a build error.
#
# Rule of thumb: JOBS <= available_RAM_in_GB / 1.5
JOBS    ?= $(shell nproc)

# Static busybox to embed in the guest initramfs.  Must be statically linked —
# an initramfs has no shared libraries, so a dynamic binary means PID 1 dies
# before it can print anything.
BUSYBOX ?= /usr/bin/busybox
