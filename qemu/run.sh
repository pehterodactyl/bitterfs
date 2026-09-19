#!/bin/sh
# ===========================================================================
# run.sh — boot the bitterfs development guest
# ===========================================================================
#
#   ./run.sh              normal boot
#   ./run.sh --gdb        wait for gdb on :1234 before executing anything
#   ./run.sh --fresh      recreate a zeroed disk image first
#   ./run.sh --snapshot   discard all disk writes when the VM exits
#   ./run.sh --smoke      run the automated checks in guest.sh, then power off
#
# There is no bootloader and no installed distro.  QEMU loads the kernel into
# guest memory and jumps to it, the kernel unpacks qemu/initramfs.cpio.gz into
# a tmpfs and execs /init from it.  That is the whole boot, and it takes about
# a second.
#
# --- getting out ------------------------------------------------------------
# With -nographic, QEMU steals Ctrl-A as an escape prefix:
#     Ctrl-A  x     quit QEMU
#     Ctrl-A  c     toggle the QEMU monitor
#     Ctrl-A  a     send a literal Ctrl-A to the guest
# That last one matters, because Ctrl-A is also "start of line" in a shell.
#
# --- notation ---------------------------------------------------------------
#   ${VAR:-default}  use $VAR if set and non-empty, otherwise "default".  This
#                    is how every path below becomes overridable from the
#                    environment without an if-statement.
#   "$@"             all positional arguments, each kept as one word even if it
#                    contains spaces.  `set -- a b c` REPLACES them, and
#                    `set -- "$@" d` appends — which is how a POSIX shell with
#                    no arrays builds up an argument list.
#   case/esac        pattern match; the ;; ends each branch.
#   exec CMD         replace this script with CMD rather than running it as a
#                    child, so Ctrl-C and exit codes pass straight through.
# ===========================================================================

set -eu

# --- Paths -----------------------------------------------------------------
# Resolved relative to this script, so run.sh works from any directory.
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)

KERNEL=${KERNEL:-$HOME/kernels/linux-6.12/linux-6.12.101}
BZIMAGE=$KERNEL/arch/x86/boot/bzImage
INITRAMFS=${INITRAMFS:-$here/initramfs.cpio.gz}
IMG=${IMG:-$here/bitter.img}
IMG_SIZE=${IMG_SIZE:-1G}
MEM=${MEM:-2G}

usage() {
	cat <<'EOF'
usage: run.sh [--gdb] [--fresh] [--snapshot]

  --gdb        freeze the guest and wait for a debugger on :1234
  --fresh      recreate a zeroed disk image before booting
  --snapshot   discard all disk writes when the VM exits
  --smoke      run guest.sh's automated checks non-interactively, then poweroff

environment overrides:
  KERNEL     kernel source tree           (default ~/kernels/linux-6.12/...)
  IMG        disk image path              (default qemu/bitter.img)
  IMG_SIZE   size when creating it        (default 1G)
  MEM        guest RAM                    (default 2G)
EOF
}

# --- Options ---------------------------------------------------------------
gdb=0
fresh=0
snapshot=0
smoke=0

while [ $# -gt 0 ]; do
	case "$1" in
	--gdb)      gdb=1 ;;
	--fresh)    fresh=1 ;;
	--snapshot) snapshot=1 ;;
	--smoke)    smoke=1 ;;
	-h|--help)  usage; exit 0 ;;
	*)
		echo "run.sh: unknown option: $1" >&2
		usage >&2
		exit 1
		;;
	esac
	shift
done

# --- Preconditions ---------------------------------------------------------
# Fail with the command that fixes it, rather than letting QEMU produce its own
# less helpful error several seconds later.
command -v qemu-system-x86_64 >/dev/null 2>&1 || {
	echo "run.sh: qemu-system-x86_64 not found — apt install qemu-system-x86" >&2
	exit 1
}
[ -f "$BZIMAGE" ] || {
	echo "run.sh: no kernel image at $BZIMAGE" >&2
	echo "        build it with: make -C '$here' kernel" >&2
	exit 1
}
[ -f "$INITRAMFS" ] || {
	echo "run.sh: no initramfs at $INITRAMFS" >&2
	echo "        build it with: make -C '$here' initramfs" >&2
	exit 1
}

# --- Disk image ------------------------------------------------------------
# A raw file, created sparse: truncate reserves no blocks until something is
# actually written, so a 1G image costs ~0 bytes on a fresh filesystem.
#
# Raw rather than qcow2 on purpose.  The file IS the device, byte for byte, so
# you can hexdump the superblock from the host while debugging mkfs without
# anything decoding a container format in between.
if [ "$fresh" = 1 ]; then
	echo "run.sh: recreating $IMG ($IMG_SIZE, zeroed)"
	rm -f "$IMG"
fi
if [ ! -f "$IMG" ]; then
	truncate -s "$IMG_SIZE" "$IMG"
	echo "run.sh: created $IMG ($IMG_SIZE, sparse)"
fi

# --- Kernel command line ---------------------------------------------------
#   console=ttyS0   printk goes to the serial port, and so does PID 1's stdio
#   nokaslr         belt-and-braces; kernel.config already disables KASLR, but
#                   an address that does not match vmlinux costs a confusing
#                   debugging session
#   panic=1         reboot 1s after a panic instead of hanging.  Combined with
#                   -no-reboot below, QEMU simply exits and hands your terminal
#                   back with the trace still on screen
#   oops=panic      promote any oops to a panic.  A filesystem that oopsed is
#                   not in a state worth continuing to poke at, and this way
#                   you always get the full stop rather than a limping kernel
#   loglevel=7      show all printk, including pr_debug from your module
cmdline="console=ttyS0 nokaslr panic=1 oops=panic loglevel=7"

# qemu/guest.sh is exec'd by /init whenever it exists on the share, so it needs
# a way to tell "a human is driving" from "run the checks and exit".  A kernel
# command-line flag is the natural channel: it is visible to the guest at
# /proc/cmdline, and it costs nothing when absent.
if [ "$smoke" = 1 ]; then
	cmdline="$cmdline bitter.auto=1"
fi

# --- Assemble the QEMU invocation ------------------------------------------
# POSIX sh has no arrays, so the positional parameters serve as the argument
# list: `set --` replaces them, `set -- "$@" ...` appends.  Argument parsing
# above already consumed the originals, so starting fresh here is safe.
#
#   -enable-kvm -cpu host   hardware virtualisation, host CPU features passed
#                           through.  Without KVM every instruction is emulated
#                           and the guest is 10-20x slower
#   -kernel / -initrd       direct boot; no bootloader, no root=, no disk read
#   -drive if=virtio        appears as /dev/vda via the paravirtualised driver
#   cache=none              O_DIRECT: keep the HOST page cache out of the
#                           durability path.  This matters more here than in
#                           most projects — see the note at the bottom
#   -virtfs mount_tag=src   the other half of the `src` in qemu/init's 9p mount
#   security_model=none     do not try to map guest uid/gid onto host files;
#                           right for a dev share, and needs no root
#   -nographic              no emulated display; serial is wired to this
#                           terminal's stdin/stdout
#   -no-reboot              exit instead of rebooting, so panic=1 returns you
#                           to the shell
set -- \
	-enable-kvm -cpu host -m "$MEM" \
	-kernel "$BZIMAGE" \
	-initrd "$INITRAMFS" \
	-append "$cmdline" \
	-drive "file=$IMG,if=virtio,format=raw,cache=none" \
	-virtfs "local,path=$repo,mount_tag=src,security_model=none" \
	-nographic \
	-no-reboot

# -snapshot redirects writes to a temporary overlay that is discarded on exit,
# leaving $IMG untouched.  Useful for experiments you do not want to keep —
# but NOT for crash-consistency testing, which is the entire point of the
# filesystem, and which needs the writes to actually persist.
if [ "$snapshot" = 1 ]; then
	set -- "$@" -snapshot
	echo "run.sh: --snapshot — disk writes will be DISCARDED on exit"
fi

# -s is shorthand for a gdb server on :1234; -S freezes the guest CPU before
# the first instruction so you can set breakpoints on early boot code.
#
# Written as an `if` rather than `[ "$gdb" = 1 ] && set -- ...` deliberately:
# under `set -e`, an AND-list that fails and is not part of a larger condition
# exits the shell, so the shorter form would quit the script whenever the flag
# was absent.
if [ "$gdb" = 1 ]; then
	set -- "$@" -s -S
	echo "run.sh: --gdb — guest is frozen, waiting for a debugger on :1234"
	echo
	echo "  in another terminal:"
	echo "    cd $KERNEL && gdb vmlinux"
	echo "    (gdb) target remote :1234"
	echo "    (gdb) lx-symbols          # after insmod, to load bitterfs.ko symbols"
	echo "    (gdb) continue"
	echo
fi

echo "run.sh: booting — Ctrl-A x to quit, Ctrl-A c for the QEMU monitor"
echo

# exec, so signals and the exit status pass straight through rather than
# arriving at a shell wrapper that then has to forward them.
exec qemu-system-x86_64 "$@"

# ===========================================================================
# A caveat to carry into phase 9
# ===========================================================================
# Killing QEMU is a WEAK crash model.  Writes that already reached the host are
# still there afterwards — the host page cache belongs to the host kernel and
# outlives the QEMU process.  So a missing flush barrier in the commit protocol
# (SKELETON.md §5) can pass this test and still lose your filesystem on real
# hardware.
#
# cache=none above gets closer to honest by bypassing the host page cache, but
# it does not close the gap.  The rigorous tool is dm-log-writes: a
# device-mapper target that records every write and every flush, then lets you
# replay the log to any point and check consistency at each one.  It is what
# btrfs and XFS developers actually use.  Note it in docs/LOG.md as a phase-9
# item; do not build it now.
#
# One hazard worth knowing about the 9p share: it exports the whole repo, so
# bitter.img is visible inside the guest at /mnt/src/qemu/bitter.img at the
# same time as it is attached as /dev/vda.  Writing through both at once will
# corrupt it in ways that look like filesystem bugs.  Use /dev/vda.
# ===========================================================================
