#!/bin/sh
# ===========================================================================
# guest.sh — phase 7 smoke checks: the write path
# ===========================================================================
# Runs as PID 1 (see qemu/init), so it must never return: it ends in poweroff
# and a sleep loop.  No `set -e` for the same reason.
#
# The load-bearing check is the REMOUNT.  chmod reporting a new mode proves
# only that setattr_copy touched the cached inode; that the value survives an
# umount and a fresh read from disk is what proves bitterfs_update_inode wrote
# it, trans_commit published it, and the superblock names the new tree.
PATH=/bin; export PATH

pass=0; fail=0
ck() { # ck <description> <expected> <actual>
	if [ "$2" = "$3" ]; then
		pass=$((pass+1)); echo "  ok   $1"
	else
		fail=$((fail+1)); echo "  FAIL $1: expected '$2', got '$3'"
	fi
}

F=/mnt/test/hello.txt

echo; echo "=== phase 7: setattr ============================"
insmod /mnt/src/bitterfs.ko || echo "insmod FAILED"
mkdir -p /mnt/test

mount -t bitterfs /dev/vda /mnt/test && echo "mount: ok"

# --- the mount is writable now ---------------------------------------------
ck "mounted rw" "rw" "$(awk '$2=="/mnt/test"{split($4,o,",");print o[1]}' /proc/mounts)"

# --- phase 6 still works ---------------------------------------------------
ck "file contents" "hello from bitterfs" "$(cat $F)"

# --- chmod -----------------------------------------------------------------
chmod 0641 $F && echo "chmod: returned 0"
ck "mode in cache" "641" "$(stat -c %a $F)"

# --- chown -----------------------------------------------------------------
chown 1000:1001 $F && echo "chown: returned 0"
ck "uid in cache" "1000" "$(stat -c %u $F)"
ck "gid in cache" "1001" "$(stat -c %g $F)"

# --- the directory takes the same path -------------------------------------
chmod 0751 /mnt/test && echo "chmod dir: returned 0"
ck "dir mode in cache" "751" "$(stat -c %a /mnt/test)"

# --- truncate is refused, not silently ignored -----------------------------
# --- THE REAL CHECK: does it survive a round trip to the disk? -------------
umount /mnt/test && echo "umount: ok"
mount -t bitterfs /dev/vda /mnt/test && echo "remount: ok"

ck "mode PERSISTED" "641" "$(stat -c %a $F)"
ck "uid PERSISTED"  "1000" "$(stat -c %u $F)"
ck "gid PERSISTED"  "1001" "$(stat -c %g $F)"
ck "dir mode PERSISTED" "751" "$(stat -c %a /mnt/test)"

# --- the tree is still intact after being written --------------------------
ck "contents after write" "hello from bitterfs" "$(cat $F)"
ck "size after write" "20" "$(stat -c %s $F)"
ck "readdir after write" "hello.txt" "$(ls /mnt/test | grep hello)"

# --- phase 7b: file DATA writes --------------------------------------------
# Appending 8192 bytes to a 20-byte file spans blocks 0, 1 and 2.  Block 0 has
# an extent already, so it exercises the mapped path; 1 and 2 have none, which
# is the only way to reach get_block's create branch -- a short append would
# land inside block 0 and allocate nothing.
dd if=/dev/zero bs=4096 count=2 2>/dev/null >> $F && echo "append: returned 0" \
	|| echo "  note append failed"
ck "appended size in cache" "8212" "$(stat -c %s $F)"

ck "first 20 bytes intact" "hello from bitterfs" "$(head -c 20 $F)"

umount /mnt/test && echo "umount: ok"
mount -t bitterfs /dev/vda /mnt/test && echo "remount: ok"

ck "appended size PERSISTED" "8212" "$(stat -c %s $F)"
ck "block count PERSISTED" "24" "$(stat -c %b $F)"
ck "first 20 bytes PERSISTED" "hello from bitterfs" "$(head -c 20 $F)"

# --- fsync ------------------------------------------------------------------
# Both of these reach bitterfs_fsync.  Without a .fsync member in the relevant
# table they are -EINVAL, not no-ops -- so the check is the exit status, and
# conv=notrunc matters because truncate is still -EOPNOTSUPP.
dd if=/dev/zero of=$F bs=1 count=1 seek=8212 conv=notrunc,fsync 2>/dev/null
ck "fsync on a file" "0" "$?"

# --- phase 7c: copy-on-write overwrite --------------------------------------
# A PARTIAL overwrite, which is the case that breaks if the CoW is done wrong:
# the untouched 15 bytes have to be read from the OLD block before the mapping
# moves.  Zero the range instead, or read it from the new block, and this check
# returns "HELLO" followed by garbage or nothing.
printf 'HELLO' | dd of=$F bs=1 seek=0 conv=notrunc 2>/dev/null
ck "partial overwrite in cache" "HELLO from bitterfs" "$(head -c 20 $F)"

umount /mnt/test && echo "umount: ok"
mount -t bitterfs /dev/vda /mnt/test && echo "remount: ok"
ck "partial overwrite PERSISTED" "HELLO from bitterfs" "$(head -c 20 $F)"

# --- phase 7d: ->create -----------------------------------------------------
# The first file this filesystem has ever made.  Every test before this one
# rides on the single hello.txt that mkfs wrote.
echo "brand new" > /mnt/test/new.txt && echo "create: returned 0"
ck "new file contents" "brand new" "$(cat /mnt/test/new.txt 2>/dev/null)"
ck "new file size" "10" "$(stat -c %s /mnt/test/new.txt 2>/dev/null)"
ck "new file has its own inode" "yes" \
	"$([ "$(stat -c %i /mnt/test/new.txt 2>/dev/null)" != "$(stat -c %i $F)" ] && echo yes)"
ck "both names listed" "hello.txt new.txt" "$(ls /mnt/test | sort | tr '\n' ' ' | sed 's/ $//')"

umount /mnt/test && echo "umount: ok"
mount -t bitterfs /dev/vda /mnt/test && echo "remount: ok"

ck "new file PERSISTED" "brand new" "$(cat /mnt/test/new.txt 2>/dev/null)"
ck "both names PERSISTED" "hello.txt new.txt" "$(ls /mnt/test | sort | tr '\n' ' ' | sed 's/ $//')"
ck "lookup finds the new name" "0" "$(test -f /mnt/test/new.txt; echo $?)"

# --- phase 7e: ->mkdir ------------------------------------------------------
# The first nested directory, and so the first time lookup and readdir are
# exercised against anything but a flat root.
mkdir /mnt/test/sub && echo "mkdir: returned 0"
ck "sub is a directory" "0" "$(test -d /mnt/test/sub; echo $?)"
ck "new dir nlink is 2" "2" "$(stat -c %h /mnt/test/sub)"
ck "parent nlink went to 3" "3" "$(stat -c %h /mnt/test)"
ck "new dir is empty" "" "$(ls /mnt/test/sub)"

# A file INSIDE it -- create against a directory that is not the root.
echo "nested" > /mnt/test/sub/deep.txt && echo "nested create: returned 0"
ck "nested file contents" "nested" "$(cat /mnt/test/sub/deep.txt 2>/dev/null)"

umount /mnt/test && echo "umount: ok"
mount -t bitterfs /dev/vda /mnt/test && echo "remount: ok"

ck "sub PERSISTED" "0" "$(test -d /mnt/test/sub; echo $?)"
ck "sub nlink PERSISTED" "2" "$(stat -c %h /mnt/test/sub)"
ck "parent nlink PERSISTED" "3" "$(stat -c %h /mnt/test)"
ck "nested file PERSISTED" "nested" "$(cat /mnt/test/sub/deep.txt 2>/dev/null)"
ck "nested listing" "deep.txt" "$(ls /mnt/test/sub)"

# --- phase 7f: truncate -----------------------------------------------------
# Its own file: truncating one the earlier checks rely on would break them.
# 12KB spans three blocks, so shrinking frees whole extents rather than just
# moving i_size.
T=/mnt/test/trunc.txt
dd if=/dev/zero bs=4096 count=3 2>/dev/null > $T
ck "3-block file size" "12288" "$(stat -c %s $T)"
ck "3-block file blocks" "24" "$(stat -c %b $T)"

# Down to one block: two extents freed.
truncate -s 4096 $T && echo "truncate down: returned 0"
ck "shrunk size" "4096" "$(stat -c %s $T)"

# Inside the first block: no extent freed, i_size just moves.
truncate -s 5 $T
ck "shrunk to 5" "5" "$(stat -c %s $T)"

# Back up past the end: a hole, with no extent behind it.
truncate -s 9000 $T
ck "grown size" "9000" "$(stat -c %s $T)"

umount /mnt/test && echo "umount: ok"
mount -t bitterfs /dev/vda /mnt/test && echo "remount: ok"

ck "truncated size PERSISTED" "9000" "$(stat -c %s $T)"
ck "hello.txt untouched" "8213" "$(stat -c %s $F)"

# --- phase 7g: ->unlink -----------------------------------------------------
# Removes a NAME.  The inode and its extents are evict_inode's job, so nothing
# here checks that space came back.
rm /mnt/test/trunc.txt && echo "unlink: returned 0"
ck "name is gone" "1" "$(test -e $T; echo $?)"
ck "not listed" "" "$(ls /mnt/test | grep trunc)"
ck "others untouched" "hello.txt new.txt sub" \
	"$(ls /mnt/test | sort | tr '\n' ' ' | sed 's/ $//')"

umount /mnt/test && echo "umount: ok"
mount -t bitterfs /dev/vda /mnt/test && echo "remount: ok"

ck "removal PERSISTED" "1" "$(test -e $T; echo $?)"
ck "others PERSISTED" "hello.txt new.txt sub" \
	"$(ls /mnt/test | sort | tr '\n' ' ' | sed 's/ $//')"
ck "nested file still there" "nested" "$(cat /mnt/test/sub/deep.txt 2>/dev/null)"

# --- phase 7h: ->evict_inode ------------------------------------------------
# unlink removes a name; evict_inode is what frees the space, when the last
# reference goes.  Written as a create/delete cycle: if the inode and its
# extents were leaking, repeating it would consume the device.
i=0
while [ $i -lt 20 ]; do
	dd if=/dev/zero bs=4096 count=4 2>/dev/null > /mnt/test/churn
	rm /mnt/test/churn
	i=$((i+1))
done
echo "churn: 20 create/delete cycles of 16KB"
ck "churn left nothing behind" "hello.txt new.txt sub" \
	"$(ls /mnt/test | sort | tr '\n' ' ' | sed 's/ $//')"

# An unlink while the file is still OPEN: the name goes immediately, the data
# stays readable until the descriptor closes.  That is the whole reason the
# deletion lives in evict_inode rather than in unlink.
echo "open-unlink" > /mnt/test/held.txt
(exec 9< /mnt/test/held.txt
 rm /mnt/test/held.txt
 ck "name gone while open" "1" "$(test -e /mnt/test/held.txt; echo $?)"
 ck "contents still readable" "open-unlink" "$(cat <&9)"
 exec 9<&-)
sync
ck "sync(1)" "0" "$?"

umount /mnt/test && echo "umount: ok"
rmmod bitterfs && echo "rmmod: ok"

echo; echo "=== $pass passed, $fail failed ==================="
echo "-- dmesg --"
dmesg | grep -iE "bitterfs|BUG|WARNING|Oops|call trace" | tail -20

poweroff -f
while true; do sleep 60; done
