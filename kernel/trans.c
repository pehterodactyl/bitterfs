// SPDX-License-Identifier: GPL-2.0
/*
 * bitterfs — when a transaction begins and ends.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/lockdep.h>
#include <linux/err.h>

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/fs.h"
#include "core/trans.h"
#include "core/bitter_assert.h"
#include "kernel/bitterfs.h"


/*
 * A write failed: say so, and stop the filesystem accepting more.
 *
 * `what` names the stage -- "commit", "inode update" -- because the difference
 * between the tree write failing and the inode write failing is most of the
 * diagnosis, and the mount's next symptom is an unrelated -EROFS with nothing
 * to explain it.
 *
 * s_id is the short device name, so two bitterfs mounts do not produce
 * identical lines.  Fires about once per mount: SB_RDONLY stops further writes
 * reaching here.
 *
 * SB_RDONLY is a stand-in for a real errored state -- nothing is recorded on
 * disk, so the next mount has no idea anything went wrong.  This is the one
 * place that changes when it arrives.  See docs/LOG.md.
 */
void bitterfs_write_failed(struct bitterfs_mount *mnt, const char *what, int err) {
  mnt->sb->s_flags |= SB_RDONLY;
  pr_err("%s: %s failed (%d), forcing read-only\n", mnt->sb->s_id, what, err);
}

/*
 * Commit the live transaction, if there is one.
 *
 * The shared half of trans_end, sync_fs and fsync: they differ entirely in WHEN
 * to commit -- a batching threshold, an explicit sync, a caller asking for
 * durability -- and not at all in what committing means.
 *
 * "If there is one" is deliberately this function's business rather than its
 * callers'.  trans_end has already asserted trans_live is set by the time it
 * arrives, so the test is redundant on that path; sync_fs is genuinely called
 * against idle filesystems, and folding it in here is what lets that function
 * be five lines.
 *
 * Caller holds mnt->lock.  Returns 0 -- including when nothing was live -- or
 * trans_commit's error.
 */
int bitterfs_commit(struct bitterfs_mount *mnt) {

  int err;

  lockdep_assert_held(&mnt->lock);

  if (!mnt->trans_live) {
    return 0;
  }

  err = trans_commit(&mnt->fs, &mnt->trans, &mnt->fs.tree_root);
  if (err < 0) {
    /*
     * On disk nothing needs undoing: the superblock was never replaced, so the
     * device still describes the previous transaction and every block this one
     * wrote is unreachable.
     *
     * The danger is the in-memory state, which is now AHEAD of the disk --
     * roots moved, refs partly drained.  Continuing would build the next
     * transaction on a foundation that is not there, and THAT commit could
     * succeed, publishing a superblock naming blocks nobody wrote.  So stop.
     *
     * trans_live stays true deliberately: there is nothing safe to do with
     * this transaction, and leaving it live means nothing tries.
     */
    bitterfs_write_failed(mnt, "commit", err);
    return err;
  }

  /* Committed: the transaction is gone.  Clearing this is what makes the next
   * begin START one rather than joining a generation that has already been
   * published and a ref set that has already been drained. */
  mnt->trans_live = false;
  return 0;
}


/*
 * Join the live transaction, or start one.
 *
 * There is at most ONE transaction per mount, and every write shares it -- so
 * this returns &mnt->trans either way and the two branches differ only in
 * whether the fields below it are initialised.  Batching is the point: a burst
 * of small operations pays for one commit between them, not one each.
 *
 * trans_joiners is NESTING DEPTH, not concurrency.  mnt->lock serialises every
 * caller, so the count rises above 1 only when one operation calls another that
 * also begins a transaction.  It is what trans_end tests to know whether it is
 * the last one out.
 *
 * trans_live is a separate flag rather than `trans_joiners > 0` because the
 * count legitimately drops to zero between two operations while the
 * transaction stays open waiting to be joined.  Conflating them would start a
 * fresh transaction per operation and discard the delayed refs of the last one
 * -- which, since the blocks are already out of the free map, hands the same
 * address to two owners.
 *
 * ERR_PTR on failure, never NULL.  Read it with IS_ERR/PTR_ERR, and note the
 * caller owes NO trans_end on that path: nothing was joined, and unwinding it
 * drives trans_joiners below zero.
 */
struct bitter_trans* bitterfs_trans_begin(struct bitterfs_mount *mnt) {

  lockdep_assert_held(&mnt->lock);

  /* Already open: join it.  Deliberately NOT re-initialising anything -- the
   * fields below hold this transaction's accumulated state. */
  if (mnt->trans_live) {
    mnt->trans_joiners++;
    return &mnt->trans; 
  }
  else {
    /* The delayed-ref array is the MOUNT's, allocated once at fill_super and
     * lent to each transaction in turn.  kvmalloc off the filesystem path
     * keeps it out of this function's failure modes, which matters because a
     * full ref set is the one -BITTER_ENOMEM that cannot be backed out of. */
    mnt->trans.refs = mnt->trans_refs;
    mnt->trans.ref_cap = BITTERFS_DELAYED_REFS;
    mnt->trans.ref_count = 0;

    /* Reads the superblock to pick up the next generation, so it can fail. */
    int err = trans_start(&mnt->env, &mnt->trans);

    if (err < 0) {
      return ERR_PTR(err);
    }

    /* AFTER trans_start succeeded, both of them.  Set before it and a failed
     * start leaves the mount believing a transaction is open. */
    mnt->trans_joiners = 1;
    mnt->trans_live = true;

    return &mnt->trans;
  }

}

/*
 * Leave the transaction, and commit it if this was the last one out AND the
 * delayed-ref set has grown near capacity.
 *
 * Two questions, and only the second is a judgement call.  Whether anyone else
 * is still inside is bookkeeping; whether to commit NOW is the batching
 * policy, and it is the whole reason this is not just a decrement.
 *
 * Deciding WHETHER is all this function does -- what a commit means lives in
 * bitterfs_commit, shared with sync_fs.
 *
 * Must be reached on every path out of an operation that began a transaction,
 * error paths included: this is the only thing that decrements trans_joiners,
 * and a count that never comes back down means no future transaction ever
 * commits.  The one exception is a failed begin, which joined nothing.
 *
 * Returns 0, or the commit's error -- by which time the mount is already
 * read-only and the failure is in dmesg.
 */
int bitterfs_trans_end(struct bitterfs_mount *mnt) {

  lockdep_assert_held(&mnt->lock);

  /* An end with no matching begin would underflow an unsigned count into a
   * very large number, which then always compares > 0 -- so nothing would ever
   * commit again, silently.  Our bug, not the disk's. */
  BITTER_ASSERT(mnt->trans_live && mnt->trans_joiners > 0);

  mnt->trans_joiners--;

  /* Nested: an outer operation is still running, and its mutations have to
   * land with the ones already made rather than being split across two
   * transactions. */
  if (mnt->trans_joiners > 0) {
    return 0;
  }

  /*
   * Not full enough yet: leave it open for the next operation to join.  This
   * is the batching, and it is the ordinary path.
   *
   * The MARGIN rather than the capacity, because an operation already under
   * way can still add refs and a full set is the one -BITTER_ENOMEM in the
   * filesystem that cannot be backed out of -- see core/trans.h.
   */
  if (mnt->trans.ref_count < BITTERFS_DELAYED_REFS - BITTERFS_DELAYED_MARGIN) {
    return 0;
  }

  /* Last one out, and the set is full enough: everything about what a commit
   * means lives in bitterfs_commit.  This function's whole job was deciding
   * WHETHER to, which is the three tests above. */
  return bitterfs_commit(mnt);
}


/*
 * ->sync_fs: force the live transaction out, whatever the batching would say.
 *
 * bitterfs_trans_end commits only once the delayed-ref set nears capacity, so
 * a lone chmod would otherwise sit in memory until some later operation filled
 * it -- and be discarded at unmount.  This is the "something else" that
 * batching always assumed would eventually force the issue.
 *
 * One method covers three callers, because they all route through
 * sync_filesystem(): sync(1), remount-to-read-only, and unmount --
 * generic_shutdown_super syncs before it calls ->put_super (fs/super.c).
 *
 * Deliberately NOT built from trans_begin/trans_end: trans_end is the thing
 * declining to commit, so going through it would reproduce the bug.
 *
 * Called with s_umount held, which orders nothing against mnt->lock -- a chmod
 * can be in flight right now.  Hence the assertion sits INSIDE the lock: by
 * the time it is held, that operation's trans_end has returned and there is
 * genuinely no joiner left.
 */
int bitterfs_sync_fs(struct super_block *sb, int wait) {

  struct bitterfs_mount* mnt = bitterfs_sb(sb);

  int err;

  /* Called twice -- (sb, 0) then (sb, 1).  The split exists so sync(1) can
   * start I/O on many filesystems before blocking on any; trans_commit is
   * synchronous and flushes the device itself, so there is no work to spread
   * across the two passes and only the blocking one acts.  An error on the
   * first pass would skip the second entirely (fs/sync.c). */
  if (wait != 1) {
    return 0;
  }

  mutex_lock(&mnt->lock);

  /* INSIDE the lock, not before it.  sync_fs is called under s_umount, which
   * orders nothing against mnt->lock -- a chmod can be in flight right now and
   * would make this read 1.  Holding the lock first waits for that operation's
   * trans_end, so by here the invariant is genuinely true. */
  BITTER_ASSERT(mnt->trans_joiners == 0);

  err = bitterfs_commit(mnt);

  mutex_unlock(&mnt->lock);
  return err;
}
