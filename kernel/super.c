// SPDX-License-Identifier: GPL-2.0
/*
 * bitterfs — mount, unmount, and module registration.
 *
 * The first file where core/ runs inside the kernel.  Everything here is VFS
 * translation: the filesystem logic is in core/ and is the same code the
 * userspace tools and tests link against.
 *
 * Replaces kernel/hello.c, whose job was to prove the build and load path.
 */

/* Must precede every kernel header: printk.h expands pr_*() using whatever
 * pr_fmt() is in scope at that point, so defining it below the includes
 * silently has no effect.  hello.c had the long version of this note. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>

#include "format/bitterfs_format.h"
#include "core/bitter_endian.h"
#include "core/bitter_crc32c.h"
#include "core/bitter_env.h"
#include "core/fs.h"
#include "core/root.h"
#include "core/extent.h"
#include "kernel/bitterfs.h"

/* Defined below, but named by get_tree above it. */
static int bitterfs_fill_super(struct super_block *sb, struct fs_context *fc);

static int bitterfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, bitterfs_fill_super);
}

/*
 * What the VFS may call on the filesystem as a whole.
 *
 * One member, and the absences are deliberate rather than unfinished:
 *
 *   .statfs       df reports nothing until there is a free-space count worth
 *                 reporting; the free map is rebuilt per mount, not stored.
 *   .write_inode  bitterfs never leaves an inode dirty -- setattr persists it
 *                 inside the same call, so writeback has nothing to collect.
 *   .put_super    kill_sb already frees what the mount owns.
 */
const struct super_operations bitterfs_super_operations = {
	.sync_fs     = bitterfs_sync_fs,

	/* Deletes an inode nothing names any more.  Required once ->unlink
	 * exists: without it every removed file leaks its inode item and all
	 * its extents, permanently and silently. */
	.evict_inode = bitterfs_evict_inode,
};

/*
 * Remount.  Both directions are now honoured: phase 7 gave the module a write
 * path, so downgrading a request for read-write would be a lie rather than the
 * honest refusal it was while nothing could write.
 *
 * sync_filesystem first, because a transition to read-only must leave nothing
 * outstanding.  That call reaches ->sync_fs, so it genuinely commits the live
 * transaction here rather than being the no-op it was before phase 7.
 */
static int bitterfs_reconfigure(struct fs_context *fc)
{
	sync_filesystem(fc->root->d_sb);
	return 0;
}

static const struct fs_context_operations bitterfs_context_ops = {
	.get_tree	= bitterfs_get_tree,
	.reconfigure	= bitterfs_reconfigure,
};

/*
 * The VFS's entry point for any mount attempt.  Everything else in this file
 * hangs off the ops table it installs.
 *
 * No .parse_param yet: bitterfs defines no mount options.  Phase 8 adds one
 * for selecting a subvolume.
 */
static int bitterfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &bitterfs_context_ops;
	return 0;
}

/*
 * Unmount.  The ordering is the whole content of this function: kill_block_super
 * runs generic_shutdown_super, which calls ->put_super while the mount is still
 * partly alive and can still read s_fs_info.  Freeing first would be a
 * use-after-free with no symptom until something else lands in that slab.
 */
static void bitterfs_kill_sb(struct super_block *sb)
{
	struct bitterfs_mount *mnt = bitterfs_sb(sb);

	kill_block_super(sb);

	/* Not part of the mount's own allocation, and kvfree rather than kfree
	 * because kvmalloc may have fallen back to vmalloc.  Safe on NULL,
	 * which is what a fill_super that failed before reaching it leaves. */
	kvfree(mnt->trans_refs);
	kvfree(mnt->free_arr);
	kfree(mnt);
}

static struct file_system_type bitterfs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "bitterfs",
	.init_fs_context	= bitterfs_init_fs_context,
	.kill_sb		= bitterfs_kill_sb,

	/* This filesystem lives on a block device, so mount(8) demands one
	 * rather than accepting a bare name. */
	.fs_flags		= FS_REQUIRES_DEV,
};

/* Lets `mount -t bitterfs` autoload the module by name. */
MODULE_ALIAS_FS("bitterfs");

/*
 * Turn a block device into a live filesystem, or refuse.
 *
 * Called by get_tree_bdev once it has a super_block with the device attached,
 * so sb->s_bdev is already live -- what it is not yet is readable, until
 * sb_set_blocksize fixes the unit sb_bread counts in.
 *
 * Still ends in a refusal, but a truthful one: every step before the last is
 * permanent, and the last becomes d_make_root once the on-disk format has an
 * FS tree to build a root inode from.
 *
 * --- cleanup -------------------------------------------------------------
 * put_super does NOT run when this fails -- generic_shutdown_super guards it
 * with `if (sb->s_root)`, and s_root is never set on a failed mount.  kill_sb
 * DOES run, via deactivate_locked_super, and it frees sb->s_fs_info.
 *
 * So assigning s_fs_info immediately after the kzalloc hands ownership of
 * `mnt` to bitterfs_kill_sb, and no path here frees it: doing so would be a
 * double free.  The only thing an error path still owes is the brelse, which
 * is what the single out_brelse label is for.
 */
static int bitterfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct bitterfs_mount	*mnt;
	struct buffer_head	*bh;
	struct bitter_super	*dsb;
	struct inode		*inode;
	bt_u32			 want, have;
	bt_u64			 last_ino;
	int			 err;

	/* A block size above PAGE_SIZE can never be set on any device, so this
	 * is a build-time truth rather than a runtime one.  4096 vs 4096 on
	 * x86-64; an arm64 kernel with 16K pages changes the margin, and a
	 * larger BITTER_BLOCK_SIZE would fail every mount for a reason no error
	 * message explains. */
	BUILD_BUG_ON(BITTER_BLOCK_SIZE > PAGE_SIZE);

	/* First, because sb_bread's unit is sb->s_blocksize and until this runs
	 * it is the device default.  Returns 0 on failure, not a negative errno
	 * -- the opposite convention to set_blocksize underneath it. */
	if (!sb_set_blocksize(sb, BITTER_BLOCK_SIZE))
		return invalfc(fc, "device does not support %d-byte blocks",
			       BITTER_BLOCK_SIZE);

	/* kzalloc, not kmalloc: fs.free_count starting at 0 is load-bearing (an
	 * allocation before extent_build_free_map must get -ENOSPC rather than
	 * read uninitialised entries), and env.ops starting NULL means a
	 * premature core/ call crashes predictably. */
	mnt = kzalloc(sizeof(*mnt), GFP_KERNEL);
	if (!mnt)
		return -ENOMEM;

	/* Both directions, for different callers: s_fs_info is how kill_sb and
	 * every future VFS callback find this, mnt->sb is how env_kernel.c
	 * reaches sb_bread.  The first line is also what makes every error
	 * return below leak nothing. */
	sb->s_fs_info = mnt;
	mnt->sb = sb;
	mutex_init(&mnt->lock);

	/* The one block that does not go through the seam.  read_block cannot
	 * read it: the superblock checksums 512 bytes rather than 4096, it is a
	 * bitter_super rather than a bitter_header so the field offsets differ,
	 * and the fsid it would be checked against is what this read supplies. */
	bh = sb_bread(sb, BITTER_SUPER_OFFSET / BITTER_BLOCK_SIZE);
	if (!bh)
		return invalfc(fc, "cannot read superblock at %#x",
			       BITTER_SUPER_OFFSET);

	dsb = (struct bitter_super *)bh->b_data;

	/*
	 * Magic first, and deliberately SILENT.  This is the "not mine" answer,
	 * and mount may be probing several filesystem types in turn; every
	 * non-bitterfs device would otherwise leave a line in dmesg.  -EINVAL
	 * with no text is how a filesystem declines a device politely.
	 *
	 * Every check below means "this IS a bitterfs and something is wrong
	 * with it", which is exactly when the user needs to be told what.
	 */
	if (memcmp(dsb->magic, BITTER_MAGIC, BITTER_MAGIC_SIZE) != 0) {
		err = -EINVAL;
		goto out_brelse;
	}

	/* BITTER_SUPER_SIZE, not BITTER_BLOCK_SIZE: the superblock's checksum
	 * stops at 512 because that is the sector the drive writes atomically,
	 * so a torn write past it must not be able to fail this. */
	want = bt_get_le32(dsb->csum);
	have = bt_block_csum(dsb, BITTER_SUPER_SIZE);
	if (want != have) {
		errorfc(fc, "superblock csum %08x, computed %08x", want, have);
		err = -EUCLEAN;
		goto out_brelse;
	}

	/* Catches a superblock read from, or written to, the wrong offset --
	 * intact and correctly checksummed, just not where it says it is. */
	if (bt_get_le64(&dsb->bytenr) != BITTER_SUPER_OFFSET) {
		errorfc(fc, "superblock claims to live at %#llx, not %#x",
			(unsigned long long)bt_get_le64(&dsb->bytenr),
			BITTER_SUPER_OFFSET);
		err = -EUCLEAN;
		goto out_brelse;
	}

	/* Stored despite being a compile-time constant precisely so a tool
	 * built with a different block size detects the mismatch instead of
	 * misparsing -- see format_super.  The kernel is the first consumer
	 * where misparsing costs a panic rather than a wrong printout. */
	if (bt_get_le32(&dsb->block_size) != BITTER_BLOCK_SIZE) {
		errorfc(fc, "image block size %u, this build expects %d",
			bt_get_le32(&dsb->block_size), BITTER_BLOCK_SIZE);
		err = -EUCLEAN;
		goto out_brelse;
	}

	/* Not damage: the field is valid and names one of the four algorithms
	 * bitterfs_format.h defines, and this build implements one of them.
	 * Same category as the feature flags, which will want checking right
	 * here once any are defined. */
	if (bt_get_le16(&dsb->csum_type) != BITTER_CSUM_TYPE_CRC32) {
		errorfc(fc, "unsupported checksum type %u",
			bt_get_le16(&dsb->csum_type));
		err = -EOPNOTSUPP;
		goto out_brelse;
	}

	/* A level past BITTER_MAX_LEVEL would overrun bitter_path's arrays on
	 * the first descent. */
	if (dsb->root_level >= BITTER_MAX_LEVEL) {
		errorfc(fc, "root level %u exceeds maximum %d",
			dsb->root_level, BITTER_MAX_LEVEL);
		err = -EUCLEAN;
		goto out_brelse;
	}

	/*
	 * Copy out everything worth keeping BEFORE the release: dsb points into
	 * the page cache, and after brelse it is not ours.  The field mapping is
	 * bitter_read_super's, which is this function's userspace counterpart.
	 */
	memcpy(mnt->fsid, dsb->fsid, BITTER_FSID_SIZE);

	mnt->fs.tree_root.level		= dsb->root_level;
	mnt->fs.tree_root.generation	= bt_get_le64(&dsb->generation);
	mnt->fs.tree_root.bytenr	= bt_get_le64(&dsb->root);
	mnt->fs.tree_root.total_bytes	= bt_get_le64(&dsb->total_bytes);

	/* The allocator's high-water mark lives in bytes_used -- see
	 * docs/LOG.md.  Read-only never allocates, so nothing consumes this
	 * yet; copied so the handle is complete rather than half-filled. */
	mnt->fs.tree_root.next_free	= bt_get_le64(&dsb->bytes_used);

	/* Not on disk: this tree is the root tree by definition. */
	mnt->fs.tree_root.objectid	= BITTER_ROOT_TREE_OBJECTID;

	mnt->fs.total_bytes		= bt_get_le64(&dsb->total_bytes);
	mnt->fs.env			= &mnt->env;
	mnt->fs.tree_root.fs_info	= &mnt->fs;

	brelse(bh);

	/*
	 * The transaction's delayed-ref array, allocated ONCE for the mount
	 * rather than once per transaction.  core/ cannot allocate -- its
	 * arrays are always caller-supplied with a count and a cap -- and doing
	 * it here rather than in trans_begin keeps a 96 KB allocation off the
	 * filesystem path, where it would be attempted under the mutex and
	 * under whatever memory pressure prompted the write.
	 *
	 * kvmalloc rather than kmalloc: nothing here is DMA'd or needs physical
	 * contiguity, and 96 KB is an order-5 allocation that can fail for
	 * fragmentation alone on a machine that has been up for a month.
	 *
	 * No failure path of its own -- kill_sb runs on a failed fill_super and
	 * kvfree(NULL) is safe, so the mount struct's ownership rule still
	 * covers it.
	 */
	mnt->trans_refs = kvmalloc_array(BITTERFS_DELAYED_REFS,
					 sizeof(*mnt->trans_refs), GFP_NOFS);
	if (!mnt->trans_refs)
		return -ENOMEM;

	/*
	 * Arm the seam, and not one line earlier.  read_block has no have_fsid
	 * gate -- it compares every block against mnt->fsid unconditionally --
	 * so installing the vtable before the memcpy above would check every
	 * block against sixteen zero bytes.  See kernel/bitterfs.h.
	 */
	bitterfs_env_init(mnt);

	/*
	 * The extent tree, which phase 5 deliberately skipped: a read-only mount
	 * allocates nothing, so nothing needed it.  Phase 7 does -- the
	 * free-space map is built by scanning it, and the first thing a write
	 * does is allocate.
	 */
	err = bitter_find_root(&mnt->env, &mnt->fs.tree_root,
			       BITTER_EXTENT_TREE_OBJECTID, &mnt->fs.extent_root);
	if (err < 0) {
		errorfc(fc, "no extent tree in this image (%d)", err);
		return -EUCLEAN;
	}
	mnt->fs.extent_root.fs_info = &mnt->fs;

	/*
	 * The free-space map, derived rather than stored: the extent tree is
	 * keyed by ADDRESS, so the gaps between its items ARE the free space and
	 * the scan is one linear walk.
	 *
	 * Its size cannot be known in advance -- it depends on how FRAGMENTED
	 * the device is, not how full -- so -BITTER_ENOMEM here means "your array
	 * was too small", not "the device is out of space", and the answer is to
	 * grow and retry.  bitter-fsck's loop has the same shape and the same
	 * ceiling: a map cannot need more entries than the device has blocks, so
	 * exceeding that is a corrupt tree rather than a small array.
	 */
	{
		bt_u32 cap = 1024;

		for (;;) {
			kvfree(mnt->free_arr);
			mnt->free_arr = kvmalloc_array(cap, sizeof(*mnt->free_arr),
						       GFP_NOFS);
			if (!mnt->free_arr)
				return -ENOMEM;

			mnt->fs.free       = mnt->free_arr;
			mnt->fs.free_cap   = cap;
			mnt->fs.free_count = 0;

			err = extent_build_free_map(&mnt->fs);
			if (err != -BITTER_ENOMEM)
				break;

			if (cap > (bt_u32)(mnt->fs.total_bytes / BITTER_BLOCK_SIZE)) {
				errorfc(fc, "free map will not fit at %u entries", cap);
				return -EUCLEAN;
			}
			cap *= 2;
		}
		if (err < 0) {
			errorfc(fc, "cannot build the free-space map (%d)", err);
			return -EUCLEAN;
		}
	}

	sb->s_maxbytes	= MAX_LFS_FILESIZE;
	sb->s_time_gran	= 1;

	/* Until now this was the VFS's all-NULL default_op, which is why every
	 * sb->s_op->... call in fs/ skipped us silently. */
	sb->s_op	= &bitterfs_super_operations;

	/*
	 * No forced SB_RDONLY.  It stood while env_kernel.c's write half was
	 * all assertions; the seam is real now, and ->setattr is the first
	 * operation that needs the VFS to let a write through at all.
	 *
	 * The paths that are still missing stay safe without it: with no
	 * ->create, ->mkdir or ->unlink the VFS answers -EACCES before reaching
	 * us, and with no ->write_iter, write(2) fails at the file operations.
	 * Absent methods refuse; they do not half-run.
	 *
	 * SB_RDONLY is now a RUNTIME verdict rather than a constant -- both
	 * bitterfs_trans_end and bitterfs_setattr set it when a write fails.
	 */

	/*
	 * The first call into core/ this module has ever made, and so the first
	 * real exercise of env_kernel.c: it reads the root tree block through
	 * read_block and checks its csum, bytenr and fsid for real.
	 *
	 * It returns -BITTER_ENOENT today because mkfs writes a root tree and an
	 * extent tree and no FS tree at all.  That is a true statement about the
	 * image rather than a stub, which is why this function ends here and why
	 * nothing above it changes when the format grows one.
	 *
	 * Reported as -EUCLEAN rather than passed through: BITTER_ENOENT is 2,
	 * which is ENOENT, and a mount failing with "no such file or directory"
	 * reads as though the DEVICE were missing.
	 */
	err = bitter_find_root(&mnt->env, &mnt->fs.tree_root,
			       BITTER_FS_TREE_OBJECTID, &mnt->fs.fs_root);
	if (err < 0) {
		errorfc(fc, "no FS tree in this image (%d)", err);
		return -EUCLEAN;
	}

	/* The same back-pointer tree_root gets, and for the same reason:
	 * anything reaching bitter_alloc_block dereferences root->fs_info.
	 * Read-only never allocates, so nothing needs it yet -- which is exactly
	 * why leaving it NULL would be a phase 7 landmine rather than a phase 5
	 * bug. */
	mnt->fs.fs_root.fs_info = &mnt->fs;

	/*
	 * Where ->create's inode numbers come from.  Derived from the tree
	 * rather than stored in the superblock: the last key in the FS tree
	 * belongs to the highest-numbered object, so one search at mount is the
	 * whole of it.
	 *
	 * Seeded here rather than lazily so that a corrupt or empty FS tree
	 * fails the MOUNT, where there is an fs_context to report through,
	 * instead of failing the first create with an errno nobody can trace.
	 *
	 * No lock: the mount is not published until s_root is set below, so
	 * nothing else can reach this field yet.
	 */
	err = bitter_find_last_objectid(&mnt->env, &mnt->fs.fs_root, &last_ino);
	if (err < 0) {
		errorfc(fc, "cannot find the last inode number (%d)", err);
		return -EUCLEAN;
	}

	/* The root directory is BITTER_FIRST_FREE_OBJECTID, so a well-formed
	 * tree can never yield less.  Anything lower means the FS tree holds
	 * only reserved-range objects, which is not a tree we can create in. */
	if (last_ino < BITTER_FIRST_FREE_OBJECTID) {
		errorfc(fc, "FS tree has no objects above the reserved range");
		return -EUCLEAN;
	}
	mnt->next_ino = last_ino + 1;

	/*
	 * The root directory.  mkfs writes its inode item at
	 * BITTER_FIRST_FREE_OBJECTID, which is 256 -- everything below that is
	 * reserved for trees.
	 */
	/*
	 * bitterfs_iget requires mnt->lock held -- see the note there.  Nothing
	 * can contend for it at this point: the mount is not published until
	 * s_root is set below, so no other task can reach these trees.  Taken
	 * anyway, so that the precondition holds uniformly and the assertion
	 * inside has no documented exception.  A precondition with one exception
	 * is a precondition nobody checks.
	 */
	mutex_lock(&mnt->lock);
	inode = bitterfs_iget(sb, BITTER_FIRST_FREE_OBJECTID);
	mutex_unlock(&mnt->lock);

	if (IS_ERR(inode)) {
		errorfc(fc, "cannot read the root directory (%ld)",
			PTR_ERR(inode));
		return (int) PTR_ERR(inode);
	}

	/*
	 * d_make_root consumes the reference bitterfs_iget returned, on success
	 * AND on failure -- it iputs the inode itself if it cannot allocate.
	 * So there is no iput here on either path.
	 *
	 * Setting s_root is what makes this superblock live.  It is also the
	 * flag generic_shutdown_super tests to decide whether to call
	 * put_super, so from here on unmount takes the full path rather than
	 * only kill_sb.
	 */
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;

out_brelse:
	brelse(bh);
	return err;
}

static int __init bitterfs_init(void)
{
	int err;

	err = register_filesystem(&bitterfs_fs_type);
	if (err) {
		pr_err("failed to register filesystem: %d\n", err);
		return err;
	}

	pr_info("registered\n");
	return 0;
}

/*
 * unregister_filesystem can fail -- it returns -EINVAL for a type that was
 * never registered -- but module_exit is void and there is nobody left to tell.
 * Reaching here at all means bitterfs_init succeeded, since a failed init means
 * the module was never loaded.
 */
static void __exit bitterfs_exit(void)
{
	unregister_filesystem(&bitterfs_fs_type);
	pr_info("unregistered\n");
}

module_init(bitterfs_init);
module_exit(bitterfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pieter Xu");
MODULE_DESCRIPTION("bitterfs: a copy-on-write B-tree filesystem");
MODULE_VERSION("0.0.1");
