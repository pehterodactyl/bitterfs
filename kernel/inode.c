// SPDX-License-Identifier: GPL-2.0

/*
 * bitterfs — turning on-disk inode items into VFS inodes.
 *
 * One direction only, for now: a read-only mount never creates an inode and
 * never writes one back.  ->create and ->mkdir arrive at phase 7.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/lockdep.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "format/bitterfs_format.h"
#include "core/bitter_endian.h"
#include "core/bitter_assert.h"
#include "core/bitter_env.h"
#include "core/btree.h"
#include "core/items.h"
#include "core/fs.h"
#include "kernel/bitterfs.h"

/*
 * Read the inode item for `ino` out of the FS tree and return a struct inode
 * for it.  ERR_PTR on failure, never NULL.
 *
 * Never called speculatively: every caller already holds an inode number from
 * somewhere authoritative -- fill_super from the format, and lookup from a
 * directory entry it just read.  That is why a missing item below is -EUCLEAN
 * and not -ENOENT: a name that does not exist is answered by lookup returning
 * a negative dentry, before this function is reached.  Absence HERE means
 * something references an inode that is not there.
 */
struct inode *bitterfs_iget(struct super_block *sb, bt_u64 ino)
{
	struct bitterfs_mount	*mnt = bitterfs_sb(sb);
	struct inode		*inode;
	struct bitter_path	 path;
	struct bitter_key_cpu	 key;
	struct bitter_inode_item disk;
	struct bitter_item	*item;
	bt_u32			 size;
	int			 s;
	int			 err;

	/* The one exit that owes nothing: no inode, so no I_NEW obligation and
	 * nothing to release.  iget_failed(NULL) would oops. */
	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	/*
	 * Already in the VFS's inode cache and fully populated.  Returning it
	 * untouched is not just an optimisation: a cached inode can hold state
	 * not yet on disk, and re-reading the item over it would silently
	 * revert that.  Harmless while read-only, which is exactly why it would
	 * survive into phase 7 unnoticed.
	 *
	 * Both paths out of iget_locked carry a reference, so the CALLER owes an
	 * iput either way.  d_make_root consumes it on success.
	 */
	if (!(inode->i_state & I_NEW))
		return inode;

	/* From here every exit must reach unlock_new_inode or iget_failed.
	 * Missing one leaves every future opener of this inode asleep on the
	 * I_NEW bit forever -- a hung task, not a crash. */

	key.objectid = ino;
	key.type     = BITTER_INODE_ITEM;
	key.offset   = 0;     /* unused for this type: an object has one inode */

	/* Mandatory before first use: btree_search releases the path on entry,
	 * and releasing an uninitialised stack path calls put_block on whatever
	 * was in that frame. */
	bitter_path_init(&path);

	/*
	 * The CALLER holds mnt->lock.  core/ is lock-free by contract and
	 * assumes its caller serialised access -- see core/bitter_env.h -- and
	 * that serialisation is acquired at the VFS entry points rather than
	 * here.
	 *
	 * Not a style preference: phase 6's lookup takes the lock, searches the
	 * directory for a name, and calls this function on the number it finds.
	 * A non-recursive mutex re-acquired by the same task is a self-deadlock,
	 * and the hung-task splat names a waiter rather than a cause.  Acquiring
	 * at the entry point and asserting in the helpers is what btrfs does,
	 * for this reason.
	 *
	 * Free when CONFIG_PROVE_LOCKING is off, and genuine when it is on --
	 * which it is in this project's config, so a caller that forgets gets a
	 * lockdep splat naming this function.
	 */
	lockdep_assert_held(&mnt->lock);

	/* ins_len 0: a pure lookup, so the descent makes no room and splits
	 * nothing.  trans NULL: this cannot write, and a null trans is what
	 * makes that structural rather than a promise. */
	s = btree_search(&mnt->env, &mnt->fs.fs_root, &key, &path, 0, NULL, 0);
	if (s != 0) {
		/* s == 1 is "not found, here is where it would go" -- normal for
		 * an insert, corruption for us.  err must NOT be s: ERR_PTR(1)
		 * is address 0x1, which IS_ERR() reports as not-an-error and the
		 * caller then dereferences. */
		err = (s < 0) ? s : -EUCLEAN;
		goto out_fail;
	}

	/*
	 * The item's size is disk-controlled, so check it before believing the
	 * payload: a short item would have this memcpy read past its end into
	 * whatever follows in the leaf.  The disk's fault, so return rather
	 * than assert -- core/bitter_assert.h's test.
	 */
	item = bitter_leaf_item(path.nodes[0]->b_data, (bt_u32)path.slots[0]);
	size = bt_get_le32(&item->size);
	if (size != BITTER_INODE_ITEM_SIZE) {
		pr_err("inode %llu: item size %u, expected %d\n",
		       (unsigned long long)ino, size, BITTER_INODE_ITEM_SIZE);
		err = -EUCLEAN;
		goto out_fail;
	}

	/* Copied out, not read in place: the pointer aims into the leaf's
	 * buffer, which the release below drops. */
	memcpy(&disk, bitter_leaf_data(path.nodes[0]->b_data,
				       (bt_u32)path.slots[0]), sizeof disk);

	bitter_path_release(&mnt->env, &path);

	/* --- fields that live on disk and nowhere else ---------------------
	 * Everything not set here was initialised by inode_init_always: i_sb,
	 * i_blkbits, i_count, and i_op/i_fop defaulting to tables that fail
	 * cleanly rather than crash.  i_ino was set by iget_locked and must not
	 * be reassigned -- the cache is keyed on it. */
	inode->i_mode = bt_get_le32(&disk.mode);

	/* Accessors, not assignment: kuid_t is a struct precisely so that
	 * skipping the user-namespace translation is a compile error. */
	i_uid_write(inode, bt_get_le32(&disk.uid));
	i_gid_write(inode, bt_get_le32(&disk.gid));

	/* set_nlink, not i_nlink = : the field is write-protected. */
	set_nlink(inode, bt_get_le32(&disk.nlink));

	inode->i_size = (loff_t) bt_get_le64(&disk.size);

	/* i_blocks counts 512-byte units regardless of block size -- the
	 * st_blocks convention -- while nbytes is a byte count. */
	inode->i_blocks = (blkcnt_t) (bt_get_le64(&disk.nbytes) / 512);

	/* u32 on disk is bt_le64; NFS uses this to detect stale handles, and
	 * generations start at 1 and rise slowly, so the truncation is
	 * deliberate rather than overlooked. */
	inode->i_generation = (u32) bt_get_le64(&disk.generation);

	inode_set_atime(inode, (time64_t) bt_get_le64(&disk.atime.sec),
			(long) bt_get_le32(&disk.atime.nsec));
	inode_set_ctime(inode, (time64_t) bt_get_le64(&disk.ctime.sec),
			(long) bt_get_le32(&disk.ctime.nsec));
	inode_set_mtime(inode, (time64_t) bt_get_le64(&disk.mtime.sec),
			(long) bt_get_le32(&disk.mtime.nsec));

	/* otime and next_dir_index are read and dropped: struct inode has no
	 * home for either, and will not until a bitterfs_inode wrapper exists.
	 * Both are safely on disk in the meantime. */

	/*
	 * Not copied from the item -- derived from the type bits in mode, since
	 * nothing on disk says which operations table to use.
	 *
	 * simple_dir_* answer readdir out of the dcache, which for a directory
	 * with genuinely no entries produces exactly "." and "..", the correct
	 * answer.  They are replaced the moment a directory can contain
	 * something.
	 */
	if (S_ISDIR(inode->i_mode)) {
		/* Both ours now: readdir walks the DIR_INDEX items on disk and
		 * lookup searches the DIR_ITEM run, rather than either being
		 * answered from the dcache. */
		inode->i_op  = &bitterfs_dir_inode_operations;
		inode->i_fop = &bitterfs_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		/* A table now, for ->setattr alone: stat still falls through to
		 * generic_fillattr, since .getattr is NULL. */
		inode->i_op  = &bitterfs_file_inode_operations;
		inode->i_fop = &bitterfs_file_operations;

		/* Replaces empty_aops, the VFS default that made cat work on a
		 * zero-byte file by never being consulted. */
		inode->i_mapping->a_ops = &bitterfs_aops;
	} else if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) ||
		   S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
		/*
		 * The only place rdev is read, and only for the two types where
		 * it means anything -- FIFOs and sockets carry no device number
		 * and pass 0.
		 *
		 * huge_decode_dev, not a raw assignment: the major/minor packing
		 * is the on-disk ABI and dev_t's internal representation is not.
		 * init_special_inode installs the right i_fop itself, which is
		 * why these four types need no table of their own.
		 */
		init_special_inode(inode, inode->i_mode,
				   huge_decode_dev(bt_get_le64(&disk.rdev)));
	} else {
		/* Symlinks are the only remaining type, and they need ->get_link
		 * plus somewhere to store the target.  Phase 10.  Until then the
		 * VFS defaults stand and open fails with -ENXIO rather than
		 * dereferencing anything. */
		pr_warn("inode %llu: unsupported mode %o\n",
			(unsigned long long)ino, inode->i_mode);
	}

	/* LAST.  This clears I_NEW and wakes every thread sleeping on it, so
	 * anything above this line must already be true. */
	unlock_new_inode(inode);
	return inode;

out_fail:
	/* Nothing may touch `inode` after iget_failed: the iput inside it may
	 * free the inode, so `err` is a value computed before this point.
	 *
	 * iget_failed ends in that iput, which can reach evict_inode -- a
	 * filesystem operation at phase 7 -- so it wants to run OUTSIDE the
	 * mutex.  It does, because the caller has not released it yet and will
	 * do so after we return.  Worth re-checking when evict_inode exists. */
	bitter_path_release(&mnt->env, &path);
	iget_failed(inode);
	return ERR_PTR(err);
}

/*
 * Write `inode`'s fields back over its on-disk INODE_ITEM.  The inverse of
 * bitterfs_iget, and the first write path in the filesystem.
 *
 * An in-place overwrite, not a remove-and-insert: the payload is a fixed 112
 * bytes under an unchanged key, so the item keeps its slot and the leaf's
 * layout does not move.  bitter_update_root has the same shape for the same
 * reason -- see core/root.c.
 *
 * Two arguments differ from iget's search: a non-null trans and cow=1.  That
 * pair is the whole difference between reading and writing, and it means the
 * descent copies every block on the way down -- so mnt->fs.fs_root may name a
 * different block when this returns.  That moved root is what trans_commit
 * publishes, which is why the real root is passed and not a copy.
 *
 * The CALLER holds mnt->lock and has an open transaction.  Returns 0, or a
 * negative errno; on failure the tree is unchanged apart from blocks the CoW
 * already copied, which stay reachable and correct.
 */
int bitterfs_update_inode(struct bitter_trans* trans, struct inode* inode) {

  struct super_block* sb = inode->i_sb;
  struct bitterfs_mount* mnt = bitterfs_sb(sb);
  struct bitter_env* env = &mnt->env;
  struct bitter_path path;
  struct bitter_key_cpu key;
  struct bitter_inode_item* disk;
  struct bitter_item *item;
  struct timespec64 t;

  void* buf;
  bt_u32 slot;
  
  bt_u64 ino = inode->i_ino;
  bt_u32 size;

  int s;
  int err;

  key.objectid = ino;
  key.type = BITTER_INODE_ITEM;
  key.offset = 0; 

  bitter_path_init(&path);

  lockdep_assert_held(&mnt->lock);

  s = btree_search(env, &mnt->fs.fs_root, &key, &path, 0, trans, 1);

  if (s != 0) {

    err = (s < 0) ? s : -EUCLEAN;
    goto out_fail;
  }

  buf = path.nodes[0]->b_data;
  slot = (bt_u32)path.slots[0];

  item = bitter_leaf_item(buf, slot);
  size = bt_get_le32(&item->size);

  if (size != BITTER_INODE_ITEM_SIZE) {
    pr_err("inode %llu: item size %u, expected %d\n",
           (unsigned long long)ino, size, BITTER_INODE_ITEM_SIZE);
    err = -EUCLEAN;
    goto out_fail;
  }

  disk = bitter_leaf_data(buf, slot);
  
  bt_put_le32(&disk->mode, inode->i_mode);
  
  bt_put_le32(&disk->uid, i_uid_read(inode));
  bt_put_le32(&disk->gid, i_gid_read(inode));
  
  bt_put_le32(&disk->nlink, inode->i_nlink);
  bt_put_le64(&disk->size, (bt_u64)inode->i_size);

  bt_put_le64(&disk->nbytes, (inode->i_blocks * 512));

  /* trans->generation, not inode->i_generation: the field names the
   * transaction writing the item, and the inode's copy is both stale and
   * truncated to 32 bits (see iget above). */
  bt_put_le64(&disk->generation, trans->generation);

  /* nsec is bt_le32, sec is bt_le64 -- bitter_timespec is 12 bytes, not 16.
   * A put_le64 into nsec spills four bytes into the NEXT timestamp, and the
   * last one would land in otime, which nothing rewrites. */
  t = inode_get_atime(inode);
  bt_put_le64(&disk->atime.sec,  (bt_u64)t.tv_sec);
  bt_put_le32(&disk->atime.nsec, (bt_u32)t.tv_nsec);

  t = inode_get_ctime(inode);
  bt_put_le64(&disk->ctime.sec,  (bt_u64)t.tv_sec);
  bt_put_le32(&disk->ctime.nsec, (bt_u32)t.tv_nsec);

  t = inode_get_mtime(inode);
  bt_put_le64(&disk->mtime.sec,  (bt_u64)t.tv_sec);
  bt_put_le32(&disk->mtime.nsec, (bt_u32)t.tv_nsec);

  /* otime, next_dir_index, rdev and flags are deliberately NOT written: they
   * have no home in struct inode, and an in-place update leaves them as they
   * were.  Building a fresh item here would zero them. */

  env->ops->dirty_block(env, path.nodes[0]);
  bitter_path_release(env, &path);
  return 0;   

out_fail:
  bitter_path_release(env, &path); 
  return err;

}

/*
 * Write a brand-new inode into the FS tree as a fresh INODE_ITEM.
 *
 * The third member of the family in this file, and the distinction between
 * them is which direction and whether the item exists:
 *
 *   bitterfs_iget           disk -> memory, item must exist
 *   bitterfs_update_inode   memory -> disk, item must exist, IN PLACE
 *   bitterfs_insert_inode   memory -> disk, item must NOT exist
 *
 * That last one is why this is btree_insert rather than btree_search: the
 * payload is built on the stack and handed over, and btree_insert picks the
 * leaf, splits whatever is full on the way down, repairs the parent separator
 * if the item lands at slot 0, dirties every block it touched, and releases
 * the path.  None of that is ours -- which is why, unlike update_inode, there
 * is no dirty_block here and no leaf pointer at all.
 *
 * It also writes all FOURTEEN fields where update_inode writes ten.  The four
 * extra ones -- generation, next_dir_index, flags and otime -- are precisely
 * the ones update_inode preserves, because on an existing item there is
 * something underneath worth keeping and on a new one there is not.  A field
 * that ever moves between those two lists has to move in both.
 *
 * Caller holds mnt->lock and an open transaction.
 */
int bitterfs_insert_inode(struct bitter_trans* trans, struct inode* inode) {

  struct super_block* sb = inode->i_sb;
  struct bitterfs_mount* mnt = bitterfs_sb(sb);
  struct bitter_env* env = &mnt->env;

  struct bitter_path path;
  struct bitter_key_cpu key;
  struct bitter_inode_item disk;
  struct timespec64 t;

  bt_u64 ino = inode->i_ino;
  int s;

  lockdep_assert_held(&mnt->lock);

  key.objectid = ino;
  key.type = BITTER_INODE_ITEM;
  key.offset = 0;   /* unused for this type: an object has one inode */

  bitter_path_init(&path);

  /* Zeroed first, so the fields not written below are defined rather than
   * whatever was on the stack.  BITTER_PACKED means no padding, which is not
   * the same as defined contents. */
  memset(&disk, 0, sizeof disk);

  /* trans->generation, not inode->i_generation: the field names the
   * transaction writing the item, and the inode's copy is a truncated u32 of
   * what the item said when it was READ -- meaningless for an item that has
   * never been read. */
  bt_put_le64(&disk.generation,     trans->generation);
  bt_put_le64(&disk.size,           (bt_u64) inode->i_size);
  bt_put_le64(&disk.nbytes,         (bt_u64) inode->i_blocks * 512);

  /* Directories only, and a high-water mark rather than a count -- readdir's
   * f_pos cookie IS this number, so it starts past the synthesised "." and
   * "..".  Nothing in struct inode can hold it, which is also why create has
   * to read the PARENT's copy back out of the tree. */
  bt_put_le64(&disk.next_dir_index,
              S_ISDIR(inode->i_mode) ? BITTER_DIR_START_INDEX : 0);

  /* le32, unlike their neighbours above and below.  A put_le64 into any of
   * these four writes over the next field and nothing warns. */
  bt_put_le32(&disk.nlink, inode->i_nlink);
  bt_put_le32(&disk.uid,   i_uid_read(inode));
  bt_put_le32(&disk.gid,   i_gid_read(inode));
  bt_put_le32(&disk.mode,  inode->i_mode);

  /* Through huge_encode_dev, never a raw i_rdev: that packing is the on-disk
   * ABI and dev_t's internal representation is not.  Zero for every type where
   * the field means nothing, which is what iget relies on when it decides
   * whether to read it at all. */
  bt_put_le64(&disk.rdev,
              (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
                      ? huge_encode_dev(inode->i_rdev) : 0);
  bt_put_le64(&disk.flags, 0);

  /* nsec is bt_le32 inside a 12-byte bitter_timespec -- a put_le64 spills four
   * bytes into the NEXT timestamp, and otime is last, so that overrun leaves
   * the item entirely. */
  t = inode_get_atime(inode);
  bt_put_le64(&disk.atime.sec,  (bt_u64) t.tv_sec);
  bt_put_le32(&disk.atime.nsec, (bt_u32) t.tv_nsec);

  t = inode_get_ctime(inode);
  bt_put_le64(&disk.ctime.sec,  (bt_u64) t.tv_sec);
  bt_put_le32(&disk.ctime.nsec, (bt_u32) t.tv_nsec);

  t = inode_get_mtime(inode);
  bt_put_le64(&disk.mtime.sec,  (bt_u64) t.tv_sec);
  bt_put_le32(&disk.mtime.nsec, (bt_u32) t.tv_nsec);

  /* otime is "created", and this inode is being created now -- mtime is that
   * same moment and there is nowhere else to get it.  The one write to this
   * field in the filesystem's life. */
  bt_put_le64(&disk.otime.sec,  (bt_u64) t.tv_sec);
  bt_put_le32(&disk.otime.nsec, (bt_u32) t.tv_nsec);

  s = btree_insert(env, &mnt->fs.fs_root, &key, &path, &disk,
                   BITTER_INODE_ITEM_SIZE, trans);

  /*
   * An item already sits at this inode number, which means next_ino handed out
   * a number the tree is already using -- fill_super's seeding under-reported,
   * or the counter drifted.
   *
   * NOT passed through as -EEXIST.  That would tell open(O_CREAT) the file
   * exists, and it does not: lookup found no such name, and what collided is
   * the number.  -EUCLEAN says the thing that is actually true.
   *
   * The in-memory half of the same check is insert_inode_locked's -EBUSY;
   * between them both places the number could already be live are covered.
   */
  if (s == -BITTER_EEXIST) {
    pr_err("inode %llu: an inode item already exists at this number\n",
           (unsigned long long) ino);
    return -EUCLEAN;
  }

  /* btree_insert released the path on every path out, success and error
   * alike -- nothing is owed here. */
  return s;
}


/*
 * The last reference to an inode is gone.  If nothing names it any more,
 * delete it and give its space back.
 *
 * --- most calls are not deletions -----------------------------------------
 * This fires for EVERY inode leaving the cache: memory pressure, unmount, the
 * root directory itself.  For those the whole body is the two VFS calls that
 * bracket the branch below, and nothing touches the disk.
 *
 * --- it returns void ------------------------------------------------------
 * No caller, no errno, no retry.  A failure can only be logged and turned into
 * SB_RDONLY -- which makes the log line the ONLY record that an inode's space
 * was leaked.  Without it, an rm that fails to free anything is completely
 * silent: rm returns 0, because unlink already succeeded in removing the name.
 *
 * --- why mnt->lock is INSIDE the branch ------------------------------------
 * bitterfs_iget's failure path calls iget_failed, which ends in iput -- with
 * the CALLER's mnt->lock still held.  During fill_super that iput evicts
 * immediately, because SB_ACTIVE is not set yet and the inode cannot go on the
 * LRU.  Taking the lock unconditionally here would self-deadlock on that path.
 *
 * It does not reach the locked branch, because such an inode has nlink 1 and
 * is a bad inode besides -- but the lock placement is what makes that true by
 * construction rather than by luck.  See docs/LOG.md, "iget_failed now runs
 * inside the caller's lock", which predicted this and deferred the decision
 * until the method existed.
 *
 * --- the guards ------------------------------------------------------------
 *   nlink == 0        the file has no names left.  The ordinary case is an
 *                     unlink whose last opener has just closed.
 *   !is_bad_inode     an inode whose read failed halfway has fields nobody
 *                     should act on; deleting tree items from them is worse
 *                     than leaking one.
 *   !sb_rdonly        a mount already frozen by a failed write must not be
 *                     written to.  Without this, an eviction after
 *                     write_failed opens a transaction on a filesystem that
 *                     has given up.
 */
void bitterfs_evict_inode(struct inode* inode) {

  struct bitterfs_mount* mnt = bitterfs_sb(inode->i_sb);
  struct bitter_trans* trans;
  bool done = false;
  int err = 0;
  int s;

  /*
   * FIRST, and unconditionally.  This truncates the PAGE CACHE, not the file:
   * it drops every folio and frees the memory, touching no disk state.  Dirty
   * folios are DISCARDED rather than written, which is exactly right when the
   * file is going away -- writing them back would be writing to blocks about
   * to be handed to the free map, which is also why it must precede the
   * deletion rather than follow it.
   */
  truncate_inode_pages_final(&inode->i_data);

  if (inode->i_nlink == 0 && !is_bad_inode(inode) && !sb_rdonly(inode->i_sb)) {
    /*
     * Batched for the same reason setattr's truncate is: one file can hold far
     * more extents than the delayed-ref set can record, and a full set is the
     * one -BITTER_ENOMEM that cannot be backed out of.  truncate_extents stops
     * at the threshold trans_end uses to decide to commit, so the loop is
     * self-regulating.
     */
    do {
      mutex_lock(&mnt->lock);

      trans = bitterfs_trans_begin(mnt);
      if (IS_ERR(trans)) {
        /* Joined nothing, so NO trans_end -- the one exit that skips it.
         * Calling it here would drive trans_joiners below zero. */
        err = PTR_ERR(trans);
        mutex_unlock(&mnt->lock);
        break;
      }

      err = bitterfs_truncate_extents(trans, inode, 0, &done);

      /*
       * The last batch finishes the job inside the same transaction, so what
       * commits is "no extents AND no inode item" rather than an inode that
       * briefly owns neither a name nor any data.
       */
      if (!err && done) {
        err = bitterfs_delete_inode_items(trans, inode);
      }

      /* Ours to report: nothing else has seen this error, and nothing will. */
      if (err) {
        bitterfs_write_failed(mnt, "evict", err);
      }

      /*
       * UNCONDITIONAL and still under the lock -- it asserts lockdep_assert_held
       * and is the only thing that decrements trans_joiners.
       *
       * Its failure is NOT re-reported: bitterfs_commit already called
       * write_failed with "commit", and a second line under "evict" would name
       * the wrong cause.  Folded into err only so the loop terminates.
       */
      s = bitterfs_trans_end(mnt);
      if (!err) {
        err = s;
      }

      mutex_unlock(&mnt->lock);

      /* Without the error test the loop never ends: a failing truncate_extents
       * leaves `done` false forever. */
    } while (!done && !err);
  }

  /*
   * LAST, and unconditionally, however the above went.  clear_inode is what
   * sets i_state to I_FREEING | I_CLEAR and hands the inode back to the VFS;
   * the generic path in evict() calls it too, and the BUG_ONs downstream show
   * how strictly the transition is policed.
   *
   * On a failed transaction the inode simply leaks -- its items and blocks
   * stay on disk.  There is nothing else available: the method returns void.
   */
  clear_inode(inode);
}


/*
 * Remove whatever is still in the tree under this inode's objectid.
 *
 * Called once, by evict_inode, after truncate_extents has taken the
 * EXTENT_DATA items and their blocks.  What remains is normally just
 * (ino, INODE_ITEM, 0) -- unlink removed the dirents and the backref with the
 * name.
 *
 * Written as "delete everything with this objectid" rather than "delete the
 * inode item", because create has a failure path that leaves more.  If it
 * inserts the inode item and the backref and then fails, discard_new_inode
 * evicts an inode with nlink == 0 whose backref is still in the tree; a
 * targeted delete would orphan it.  The general form costs one extra search
 * and never has to decide whether an absence is suspicious.
 *
 * Unbounded in principle, bounded in practice: after the extents are gone
 * there are at most a handful of items left, and none of them owns a block, so
 * nothing here records a delayed ref and the set cannot overflow.  That is why
 * this needs no batching where truncate_extents did.
 *
 * Caller holds mnt->lock and an open transaction.
 */
int bitterfs_delete_inode_items(struct bitter_trans *trans, struct inode *inode)
{
	struct bitterfs_mount	*mnt = bitterfs_sb(inode->i_sb);
	struct bitter_env	*env = &mnt->env;

	struct bitter_path	 path;
	struct bitter_key_cpu	 probe;
	struct bitter_key_cpu	 leaf_key;
	struct bitter_item	*it;
	bt_u64			 ino = inode->i_ino;
	int			 s;

	lockdep_assert_held(&mnt->lock);

	/* A key nothing can match, so the descent lands one slot past this
	 * inode's last item.  0xFF because type dominates offset -- a lower one
	 * would sort before the items being looked for. */
	probe.objectid = ino;
	probe.type     = 0xFF;
	probe.offset   = ~0ULL;

	for (;;) {
		bitter_path_init(&path);

		s = btree_search(env, &mnt->fs.fs_root, &probe, &path, 0,
				 NULL, 0);
		if (s < 0)
			return s;          /* search released the path itself */

		/* Nothing precedes the probe at all: an empty tree, which for a
		 * real filesystem means the root item is gone too.  Treated as
		 * "nothing left to delete" rather than an error -- this function
		 * only promises the inode is absent, and it is. */
		if (path.slots[0] == 0) {
			bitter_path_release(env, &path);
			return 0;
		}

		it = bitter_leaf_item(path.nodes[0]->b_data,
				      (bt_u32)(path.slots[0] - 1));
		bitter_key_from_disk(&leaf_key, &it->key);

		bitter_path_release(env, &path);

		/* Stepped off this inode's range: done. */
		if (leaf_key.objectid != ino)
			return 0;

		/*
		 * An EXTENT_DATA here would own a block, and deleting it without
		 * recording a -1 would leak that block permanently.  Cannot
		 * happen -- truncate_extents ran to completion first -- but the
		 * check is one comparison and the alternative is silent.
		 */
		if (leaf_key.type == BITTER_EXTENT_DATA) {
			pr_err("inode %llu: extent item survived truncation\n",
			       (unsigned long long) ino);
			return -EUCLEAN;
		}

		s = btree_del_item(env, &mnt->fs.fs_root, &leaf_key, &path,
				   trans);
		if (s < 0) {
			/*
			 * -ENOENT means the item vanished between the search
			 * and the delete, which nothing else can do while
			 * mnt->lock is held.  Still not worth distinguishing:
			 * this function's promise is that the inode is not on
			 * disk, and an item that is already gone satisfies it.
			 */
			if (s == -BITTER_ENOENT)
				return 0;
			return s;
		}
	}
}

/*
 * chmod, chown and utimes: apply the request to the inode and persist it.
 *
 * The first VFS entry point that writes, so unlike every helper it ACQUIRES
 * mnt->lock rather than asserting it, and it owns the transaction.
 *
 * Three steps, and the order of the middle two matters.  setattr_prepare does
 * the permission checks; trans_begin comes next; only THEN does setattr_copy
 * touch the inode.  Copying first would mutate the cached inode with no
 * transaction to persist it and nothing to roll it back -- disk and memory
 * disagreeing, through a path where it is entirely avoidable.
 *
 * The VFS holds i_rwsem exclusive across this call, which is what makes
 * reading the inode's fields in update_inode safe.  Lock order is i_rwsem
 * outside, mnt->lock inside, everywhere.
 */
int bitterfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
      struct iattr *iattr) {

  struct inode* inode = d_inode(dentry);
  struct bitterfs_mount* mnt = bitterfs_sb(inode->i_sb);
  struct bitter_trans* trans;

  int err;
  int r;

  /* Ownership and capability checks -- whether this caller may make this
   * change at all.  Propagated unchanged; no lock needed, since it reads the
   * inode under the i_rwsem the VFS already holds. */
  err = setattr_prepare(idmap, dentry, iattr);
  if (err)
    return err;

  /*
   * ATTR_SIZE is truncate, and it is handled BEFORE the ordinary attribute
   * path below because it cannot fit in one transaction: a large file has more
   * extents than the delayed-ref set can record, so the freeing runs as a loop
   * of bounded batches with a commit between them.
   *
   * setattr_copy never touches i_size, so nothing below would do this.
   */
  if (iattr->ia_valid & ATTR_SIZE) {
    bool done = false;

    /* RLIMIT_FSIZE and the loff_t bounds.  Not part of setattr_prepare, which
     * checks permission rather than size. */
    err = inode_newsize_ok(inode, iattr->ia_size);
    if (err)
      return err;

    /*
     * Sets i_size AND zeroes the tail of the last partial folio, leaving it
     * dirty so writeback persists the zeros.  That is what stops a later grow
     * from exposing the old bytes -- truncate to 100, back to 4096, and
     * 100..4095 must read as zeros.
     *
     * Before the loop, and deliberately: a crash mid-truncate then leaves
     * extents PAST the end of the file, which is leaked space a rescan can
     * find.  The other order leaves an i_size claiming bytes whose extents are
     * gone, which reads back as silent zeros.
     *
     * Outside mnt->lock, because it can sleep and touches the page cache.
     * Growing a file needs nothing else: the range past the old end has no
     * extent items, and get_block already reports that as a hole.
     */
    truncate_setsize(inode, iattr->ia_size);

    while (!done) {
      struct bitter_trans *t;
      int r2;

      mutex_lock(&mnt->lock);

      t = bitterfs_trans_begin(mnt);
      if (IS_ERR(t)) {
        mutex_unlock(&mnt->lock);
        err = PTR_ERR(t);
        break;
      }

      err = bitterfs_truncate_extents(t, inode, (bt_u64) iattr->ia_size,
                                      &done);

      /* Unconditional, as everywhere: the only thing that decrements
       * trans_joiners.  It commits when the batch filled the ref set, which is
       * the same threshold the helper stopped at. */
      r2 = bitterfs_trans_end(mnt);
      if (!err)
        err = r2;

      mutex_unlock(&mnt->lock);

      if (err) {
        bitterfs_write_failed(mnt, "truncate", err);
        return err;
      }
    }

    if (err)
      return err;

    /* Falls through: i_size is set and the extents are gone, but the inode
     * item still has to be written, which the ordinary path below does. */
  }

  mutex_lock(&mnt->lock);

  trans = bitterfs_trans_begin(mnt);
  if (IS_ERR(trans)) {
    /* The one exit that must NOT call trans_end: a failed begin joined
     * nothing, and unwinding it would drive trans_joiners below zero. */
    err = PTR_ERR(trans);
    goto out_unlock;
  }

  /* In-memory only, and void: it walks ia_valid, translates uid/gid through
   * the idmap, and applies the S_ISGID-stripping rule.  Nothing reaches the
   * disk until update_inode. */
  setattr_copy(idmap, inode, iattr);

  err = bitterfs_update_inode(trans, inode);

  /*
   * UNCONDITIONAL, on the error path too: trans_end is the only thing that
   * decrements trans_joiners, and a count that never comes back down means no
   * future transaction ever commits.
   *
   * err wins over r because it is the cause; r is a consequence or a second,
   * unrelated failure.  A commit that succeeds after update_inode failed
   * publishes a valid tree without the change in it -- which is why this
   * error must still reach userspace intact.
   */
  r = bitterfs_trans_end(mnt);
  if (!err)
    err = r;

  /*
   * Not because the damage is severe -- the tree stays valid -- but because
   * setattr_copy has already changed the cached inode, so memory and disk now
   * disagree with no error left anywhere to find them by.  Going read-only
   * stops that stale value propagating into anything further.
   *
   * Through the shared helper, so "a write failed" has one answer rather than
   * one per function that noticed.
   */
  if (err)
    bitterfs_write_failed(mnt, "inode update", err);

out_unlock:
  mutex_unlock(&mnt->lock);
  return err;
}

