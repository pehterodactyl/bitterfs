/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bitterfs — types shared between the module's .c files.
 *
 * One header for the whole module rather than one per source file, which is
 * what ext2 (ext2.h), efs (efs.h), exfat and erofs all do.  A per-file header
 * earns its place only when a .c grows functions others call; super.c has none
 * yet, and neither does this file's caller list.
 *
 * Created at the moment a SECOND file needed struct bitterfs_mount --
 * env_kernel.c reaching mnt->sb to call sb_bread.
 */
#ifndef BITTERFS_KERNEL_H
#define BITTERFS_KERNEL_H

#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/fs.h"
#include "core/trans.h"
/*
 * Everything one mounted bitterfs owns.  Hung off sb->s_fs_info, allocated by
 * fill_super and freed by kill_sb.
 *
 * `sb` is the back-pointer that unties the knot between the other two members:
 * env_kernel.c's read_block needs a struct super_block * for sb_bread, and
 * reaches it from here via env.priv, which points at this whole struct.  One
 * allocation, so neither member has to exist before the other.
 *
 * `fsid` is the environment's copy of the filesystem's identity, read from the
 * superblock at mount.  It is here rather than read from disk per block for
 * the same reason env_user keeps one: read_block compares every block against
 * it, and the superblock is not readable through the seam.
 *
 * ORDERING CONSTRAINT: fill_super must fill `sb` and `fsid` BEFORE it installs
 * env.ops, because read_block reads both and has no gate of its own.  Nothing
 * enforces this but fill_super itself.
 *
 * `lock` is the single filesystem-wide mutex core/bitter_env.h describes --
 * core/ never locks and assumes its caller has already serialised access.  A
 * mutex rather than a spinlock is not a preference: sb_bread sleeps.
 */
/*
 * How many delayed refs one transaction can hold, and how much headroom
 * trans_end keeps in front of that.
 *
 * The capacity is a batching knob: bigger means more operations per commit,
 * and 4096 entries is 96 KB per mount -- paid once at fill_super, not per
 * transaction.
 *
 * The MARGIN is a correctness bound, and the reason it exists is in
 * core/trans.h: a full delayed-ref set returns -BITTER_ENOMEM, and unlike
 * every other ENOMEM in the filesystem that one is UNRECOVERABLE, because an
 * allocation that is never recorded is a block handed out twice.
 *
 * So trans_end commits when ref_count comes within MARGIN of capacity, not
 * when it reaches it -- an operation already under way can still add refs, and
 * discovering the set is full halfway through a create is exactly the case
 * that cannot be backed out of.  128 covers a worst-case create: four tree
 * insertions, each copy-on-writing a path up to BITTER_MAX_LEVEL deep, each
 * CoW costing one allocation and one free.
 */
#define BITTERFS_DELAYED_REFS    4096
#define BITTERFS_DELAYED_MARGIN   128

struct bitterfs_mount {
	struct super_block	*sb;
	struct bitter_env	 env;
	struct bitter_fs_info	 fs;
	struct mutex		 lock;
	bt_u8			 fsid[BITTER_FSID_SIZE];

  /* The one open transaction, and whether there is one.  `trans_live` is
   * separate from the joiner count on purpose: the count drops to zero
   * between operations while the transaction stays open, which is what
   * batching means. */
  struct bitter_trans      trans;
  bool                     trans_live;

  /* Nesting depth, not concurrency -- the mutex means two operations
   * never overlap.  It answers one question for trans_end: am I the last
   * one out, and therefore is it safe to consider committing? */
  unsigned int             trans_joiners;

  /* The delayed-ref array, allocated once per MOUNT rather than per
   * transaction: core/ cannot allocate, and doing it here keeps the
   * kvmalloc off the filesystem path and out of trans_begin's failure
   * modes.  Allocated in fill_super, freed in kill_sb. */
  struct bitter_delayed_ref *trans_refs;

  /* The free-space map's storage, borrowed by core/ for the same reason
   * trans_refs is.  Its size is not knowable in advance -- it depends on how
   * fragmented the device is, not how full -- so fill_super grows it until the
   * scan fits, exactly as bitter-fsck does. */
  struct bitter_free_extent *free_arr;

  /*
   * The next inode number ->create will hand out.
   *
   * Seeded at mount by walking to the last key in the FS tree: keys sort
   * (objectid, type, offset) with objectid first, so the final item in the
   * whole tree belongs to the highest-numbered object.  Derived rather than
   * stored, which is why no superblock field had to be spent on it.
   *
   * The consequence of deriving it: a number is reused if the highest-numbered
   * inode is deleted and the filesystem remounted.  Harmless locally -- the
   * name is gone and nothing refers to the number -- but it is what
   * i_generation exists to disambiguate for NFS, and it is the reason a
   * superblock counter would be the stricter choice.
   *
   * Protected by `lock`, like everything else here.
   */
  bt_u64                   next_ino;
};

/*
 * s_fs_info is a void *, so every use of it is an unchecked cast.  This makes
 * one place responsible for getting it right, which is why btrfs has
 * btrfs_sb() and efs has SUPER_INFO() in exactly this form.
 */
static inline struct bitterfs_mount *bitterfs_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

/*
 * Point `mnt->env` at the kernel's implementation of the seam.
 *
 * Defined in env_kernel.c, which keeps its ops table static -- this is the
 * only handle super.c gets on it.  Two assignments, but they belong beside the
 * table rather than beside the caller: adding an operation should not mean
 * editing fill_super.
 *
 * Sets env.priv to `mnt` itself, which is what makes bitterfs_mount reachable
 * from every callback.  Call it only after `sb` and `fsid` are filled in --
 * see the ordering note on the struct above.
 */
void bitterfs_env_init(struct bitterfs_mount *mnt);

/*
 * Read the inode item for `ino` out of the FS tree and return a VFS inode for
 * it.  ERR_PTR on failure, never NULL -- so callers test with IS_ERR(), and
 * must not kfree or dereference the result without doing so.
 *
 * Defined in kernel/inode.c.  Takes mnt->lock itself for now; see the note
 * there about phase 6 moving it to the VFS entry points.
 */
struct inode *bitterfs_iget(struct super_block *sb, bt_u64 ino);

/*
 * The inverse: persists a VFS inode back to its INODE_ITEM.  Caller holds
 * mnt->lock and supplies the open transaction -- a function that takes a trans
 * can write, one that cannot take one provably cannot.
 */
int bitterfs_update_inode(struct bitter_trans *trans, struct inode *inode);

/*
 * The other memory-to-disk direction: writes a BRAND-NEW inode as a fresh
 * INODE_ITEM.  update_inode overwrites an existing item in place and preserves
 * four fields it cannot know; this one writes all fourteen, because on a new
 * inode there is nothing underneath to preserve.
 */
int bitterfs_insert_inode(struct bitter_trans *trans, struct inode *inode);

/*
 * Free the extents past `new_size`, up to as many as this transaction can
 * still record; *done says whether the file is fully trimmed.  Bounded because
 * one truncate can free more blocks than the delayed-ref set holds -- see the
 * definition in kernel/file.c.
 */
int bitterfs_truncate_extents(struct bitter_trans *trans, struct inode *inode,
                              bt_u64 new_size, bool *done);

/*
 * Remove whatever is left in the tree under this inode's objectid, after
 * truncate_extents has taken the data.  Deletes by objectid rather than by
 * named key, so create's partial-failure residue is covered too.
 */
int bitterfs_delete_inode_items(struct bitter_trans *trans, struct inode *inode);

/*
 * ->evict_inode.  Fires for every inode leaving the cache, and deletes only
 * when nothing names it any more.  Returns void: a failure can be logged and
 * nothing else.
 */
void bitterfs_evict_inode(struct inode *inode);

/*
 * ->setattr, shared by both inode_operations tables: chmod, chown and utimes
 * are the same operation whatever the file's type.
 */
int bitterfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                     struct iattr *iattr);

/*
 * Directory operations, defined in kernel/dir.c.  The TABLES cross the file
 * boundary rather than the methods: bitterfs_iget installs them, and the
 * methods themselves are static because only the VFS calls them, through these.
 */
extern const struct super_operations bitterfs_super_operations;
extern const struct file_operations bitterfs_dir_operations;
extern const struct inode_operations bitterfs_dir_inode_operations;

/* Regular files, defined in kernel/file.c. */
/*
 * ->fsync, shared by both file_operations tables: committing is filesystem-wide
 * here, so a directory and a file want exactly the same call.
 */
int bitterfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);

extern const struct file_operations bitterfs_file_operations;
extern const struct inode_operations bitterfs_file_inode_operations;
extern const struct address_space_operations bitterfs_aops;

/*
 * Join the running transaction, starting one if there is none.  ERR_PTR on
 * failure, never NULL.
 *
 * Every caller holds mnt->lock, and that is what makes the whole lifecycle
 * simple: two operations never overlap, so there is no waiting, no joining
 * protocol and no transaction state machine -- which is most of why btrfs's
 * equivalent is two hundred lines and this is fifteen.
 */
struct bitter_trans *bitterfs_trans_begin(struct bitterfs_mount *mnt);

/*
 * Leave the transaction, committing it if this was the last joiner AND the
 * delayed-ref set has come within BITTERFS_DELAYED_MARGIN of full.
 *
 * Returning 0 with the transaction still open is the ORDINARY case, and the
 * reason the lifecycle exists: without it every create would be a superblock
 * write and two device flushes.
 */
int bitterfs_trans_end(struct bitterfs_mount *mnt);

/*
 * ->sync_fs.  Commits the live transaction regardless of how full the delayed
 * ref set is -- the override that makes a write durable before the batching
 * threshold would have fired.
 */
int bitterfs_sync_fs(struct super_block *sb, int wait);

/*
 * Commit the live transaction, if there is one, and apply the failure policy
 * either way.  Caller holds mnt->lock.
 *
 * The shared half of trans_end, sync_fs and fsync: they differ entirely in WHEN
 * to commit and not at all in what committing means.
 */
int bitterfs_commit(struct bitterfs_mount *mnt);

/*
 * Report a failed write and force the mount read-only.  `what` names the stage
 * for the log line.  The single place the failure policy lives, so that the
 * errored state replacing SB_RDONLY lands in one edit rather than three.
 */
void bitterfs_write_failed(struct bitterfs_mount *mnt, const char *what, int err);
#endif /* BITTERFS_KERNEL_H */
