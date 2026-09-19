#!/bin/sh
# ===========================================================================
# mkinitramfs.sh — pack the bitterfs guest root filesystem
# ===========================================================================
#
# Emits a gen_init_cpio spec (static entries plus one symlink per busybox
# applet) and packs it into a gzipped cpio archive that QEMU hands the guest
# via -initrd.
#
# The kernel unpacks that archive into a tmpfs, makes it /, and execs /init as
# PID 1.  No disk is involved in booting: the bitterfs image on /dev/vda is a
# DATA disk mounted on demand, never the root.  That separation is the point —
# when bitterfs corrupts itself in phase 7, the VM still boots and you still
# have a shell to run bitter-fsck from.
#
# Why gen_init_cpio rather than `cpio -o` over a staging directory: device
# nodes.  mknod needs CAP_MKNOD, and building a dev image under sudo is how you
# end up with root-owned files in your repo.  gen_init_cpio synthesises nodes,
# ownership and modes INSIDE the archive from a text spec, needing no privilege
# on the host at all.  It ships in the kernel tree and is built as a side
# effect of building the kernel.
#
# Contract (see qemu/Makefile):
#   env   KERNEL   kernel source tree; we use $KERNEL/usr/gen_init_cpio
#         BUSYBOX  path to a STATIC busybox binary to embed
#         INIT     path to the /init script to embed as PID 1
#   argv  $1       output path for the gzipped cpio
#
# Usage:
#   make -C qemu initramfs                                    (normal path)
#   KERNEL=... BUSYBOX=... INIT=... ./mkinitramfs.sh out.cpio.gz
# ===========================================================================

set -eu

# --- Inputs ----------------------------------------------------------------
# `set -u` aborts on an UNSET variable but lets an EMPTY one through, and an
# empty KERNEL would produce a confusing complaint about "/usr/gen_init_cpio"
# — a path nobody configured.  ${VAR:?msg} catches both cases and names the
# variable that is actually wrong.
: "${KERNEL:?not set — normally supplied by qemu/Makefile}"
: "${BUSYBOX:?not set — path to a static busybox binary}"
: "${INIT:?not set — path to the /init script to embed}"

out=${1:?usage: mkinitramfs.sh <output.cpio.gz>}

gen_init_cpio="$KERNEL/usr/gen_init_cpio"

# The asymmetry in these three checks is deliberate, not an oversight:
#
#   gen_init_cpio  must be EXECUTABLE — we run it.
#   $INIT          only needs to be READABLE.  We copy its bytes; the 0755 that
#                  makes it runnable comes from the mode field in the spec line
#                  further down, applied inside the archive.  Its mode on the
#                  host is irrelevant.
#   $BUSYBOX       same — readable is enough.  The property that actually
#                  matters (statically linked) is checked by the Makefile,
#                  because a dynamic busybox yields an initramfs that panics
#                  with nothing useful on the console.
#
# -x is paired with -f on purpose: for a directory, -x merely means
# "searchable", so -x alone would pass for any directory you can cd into.
[ -f "$gen_init_cpio" ] && [ -x "$gen_init_cpio" ] || {
	echo "mkinitramfs: no gen_init_cpio in $KERNEL - run: make -C qemu kernel" >&2
	exit 1
}
[ -f "$INIT" ] || { echo "mkinitramfs: no init script at $INIT" >&2; exit 1; }
[ -f "$BUSYBOX" ] || { echo "mkinitramfs: no busybox at $BUSYBOX" >&2; exit 1; }

# --- Scratch state ---------------------------------------------------------
# The spec is generated wholesale rather than kept as a checked-in file with
# placeholder paths: the `file` lines need absolute HOST paths, and sed-ing
# placeholders into a template is a worse reimplementation of the shell
# expansion an unquoted heredoc gives us for free.
spec=$(mktemp)
trap 'rm -f "$spec" "$out.tmp"' EXIT

# --- Static entries --------------------------------------------------------
# Everything between `cat > "$spec" <<EOF` and `EOF` is SPEC TEXT, not shell
# commands — it is the description of the archive, consumed by gen_init_cpio.
# The delimiter is UNQUOTED (<<EOF, not <<'EOF') so that $INIT and $BUSYBOX
# expand into absolute host paths.  That expansion is the whole reason this
# script generates the spec instead of storing it.
#
# Order is a hard requirement: gen_init_cpio writes entries in the order given
# and the kernel unpacks them in that order, so every parent directory must
# appear before anything created inside it.  /bin in particular must exist
# before the ~400 symlinks below land in it.
#
# Why these device nodes, when /init mounts devtmpfs anyway: they must exist in
# the window BEFORE that mount happens.
#
#   /dev/console (c 5 1) — the kernel opens this to give PID 1 its stdin,
#                          stdout and stderr.  Missing, you get "unable to open
#                          an initial console" and a shell you cannot type into.
#   /dev/null    (c 1 3) — things redirect to it before devtmpfs is up.
#   /dev/tty     (c 5 0) — the controlling terminal; cttyhack and job control
#                          want it.
#
# NOTE: CONFIG_DEVTMPFS_MOUNT does not help here.  Its help text says the
# kernel mounts devtmpfs before calling init, which holds only when booting a
# real root filesystem — explicitly not for initramfs.  /init must mount it by
# hand, and forgetting is why /dev/vda goes missing despite virtio-blk being
# compiled in.
cat > "$spec" <<EOF
# --- directories (parents first) ---
dir /proc     0755 0 0
dir /sys      0755 0 0
dir /dev      0755 0 0
dir /tmp      1777 0 0
dir /root     0700 0 0
dir /mnt      0755 0 0
dir /mnt/src  0755 0 0
dir /bin      0755 0 0

# --- device nodes needed before devtmpfs is mounted ---
nod /dev/console 0600 0 0 c 5 1
nod /dev/null    0666 0 0 c 1 3
nod /dev/tty     0666 0 0 c 5 0

# --- payload ---
file /init        $INIT    0755 0 0
file /bin/busybox $BUSYBOX 0755 0 0
EOF

# --- Busybox applet symlinks -----------------------------------------------
# One static binary provides ~400 commands, dispatching on argv[0].  Each needs
# its own symlink, or `mount`, `insmod` and `dmesg` are simply not commands.
#
# We cannot rely on busybox's shell resolving applets by itself: that happens
# only if it was compiled with FEATURE_SH_STANDALONE, which distro builds do
# not guarantee.  Generating the links costs nothing — a symlink in a cpio is a
# few dozen bytes.
#
# The link target is RELATIVE ("busybox", not "/bin/busybox"), matching what
# `busybox --install -s` produces; it resolves within /bin wherever the archive
# is unpacked.
#
# Captured into a variable first, deliberately: a pipeline's exit status is the
# LAST command's, so `busybox --list | while ...` would silently swallow a
# failure of busybox itself.  A failing command substitution in an assignment
# does trip `set -e`.
applets=$("$BUSYBOX" --list)

echo "$applets" | while read -r applet; do
	# Skip an applet named "busybox" if the list contains one.  Emitting
	# `slink /bin/busybox busybox` would put a self-referential symlink at the
	# same path as the real binary above — and depending on which entry wins,
	# NOTHING in the guest executes.
	#
	# Written as `case` rather than `[ ... ] && continue` on purpose: under
	# `set -e` a failing AND-list that is not part of a larger condition exits
	# the shell, so the idiomatic-looking version dies on the first applet that
	# is not "busybox".
	case "$applet" in
	busybox) continue ;;
	esac
	echo "slink /bin/$applet busybox 0777 0 0"
done >> "$spec"

# --- Pack ------------------------------------------------------------------
# Written to a sibling temp path and renamed into place.  A rename within a
# directory is atomic, so an interrupted or failed pack cannot leave a
# truncated archive that boots halfway and panics confusingly: you have either
# the previous good initramfs or the new one, never a torn mixture.
#
# Same reasoning as the superblock flip in SKELETON.md §5, at a vastly smaller
# scale — and worth noticing that the shape is identical.  Stage the new state
# somewhere harmless, then publish it with one atomic operation.
"$gen_init_cpio" "$spec" | gzip -9 > "$out.tmp"
mv "$out.tmp" "$out"

echo "mkinitramfs: $(echo "$applets" | wc -l) busybox applets, init=$INIT"
echo "mkinitramfs: wrote $out"
echo "mkinitramfs: inspect with — zcat $out | cpio -itv | head -30"
