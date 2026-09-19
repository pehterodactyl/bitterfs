#ifndef BITTER_TRANS_H
#define BITTER_TRANS_H


#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/fs.h"


struct bitter_delayed_ref {
  bt_u64 bytenr;
  bt_u64 length;
  int delta;

};

struct bitter_trans {

  bt_u64 generation;

  struct bitter_delayed_ref* refs;
  bt_u32 ref_count;
  bt_u32 ref_cap;

};

/*
 * Records that an extent's reference count should change by `delta` when this
 * transaction commits.
 *
 * Takes no env, and that is the whole point: reaching the disk would allocate,
 * and allocating records another reference.  This function can only touch
 * memory, so the recursion is impossible rather than merely avoided.  The
 * changes are applied later by extent_apply_delayed_refs.
 *
 * Deferring is safe because the caller has ALREADY taken the block out of the
 * in-memory free map, so nothing else can be handed the same address before
 * the set is drained.
 *
 * Merges on an EXACT match of both bytenr and length -- an extent item is
 * keyed (bytenr, EXTENT_ITEM, length), so a contained range is a different
 * item, not the same one.  A merge that nets to zero removes the entry
 * entirely: a block allocated and freed inside one transaction never reaches
 * the extent tree at all.
 *
 * A miss is the ordinary case.  The set starts empty at every transaction, so
 * a first touch has nothing to merge with.
 *
 * Returns 0, or -BITTER_ENOMEM when the set is full.  Unlike every other
 * ENOMEM in the filesystem this one is NOT recoverable: a full free map can be
 * rebuilt at the next mount, so losing a range merely leaks space, but an
 * allocation that is never recorded is a block handed out twice.  A caller
 * that sees it must abort the transaction.  (A merge still succeeds when the
 * set is full, since it needs no new slot.)
 */
int trans_add_delayed_ref(struct bitter_trans* trans, bt_u64 bytenr, bt_u64 length, int delta);


/*
 * Reads the superblock through the seam and checks it is one.
 *
 * Magic first and silently, so a device that is not bitterfs gets a clean
 * -BITTER_EINVAL rather than a complaint about a checksum that was never ours.
 * Everything after it means "this IS a bitterfs and something is wrong with
 * it" and reports -BITTER_EUCLEAN.
 *
 * The checksum covers BITTER_SUPER_SIZE and not the whole block: 512 bytes is
 * the sector a drive writes atomically, and with one copy and no mirrors a
 * torn write beyond it must not be able to fail this check.
 *
 * `sb` is filled only on success, so a refusal leaves the caller's buffer
 * untouched.
 */
int bitter_read_super_raw(struct bitter_env* env, struct bitter_super* sb);

/*
 * Fills `root` with the root tree, as the superblock describes it.
 *
 * Nothing touches `root` until the superblock has passed its checks, so a
 * refusal leaves the caller's handle exactly as it was.
 */
int bitter_read_super(struct bitter_env* env, struct bitter_root* root);

/*
 * Opens a transaction: reads the committed generation and sets this one to the
 * next.  The delayed-ref set is the CALLER's -- core/ cannot allocate -- and
 * must already be wired up with a capacity.
 */
int trans_start(struct bitter_env* env, struct bitter_trans* trans);

/*
 * Applies the transaction's delayed refs, records the extent tree's moved
 * root, then makes the whole thing durable with the superblock write.
 *
 * `root` is the root tree being committed and is MUTATED: recording the extent
 * tree's new root copy-on-writes the root tree, so its own address moves, and
 * that moved address is what reaches super.root.
 *
 * `fs` supplies the environment, the extent tree, and the free-space map the
 * copy-on-writes allocate from.  It must be the filesystem `root` belongs to;
 * nothing checks that.
 *
 * --- the ordering, which is the whole point -------------------------------
 * The superblock's `root` is THE pointer: everything is reachable from it, and
 * replacing it is the moment the transaction becomes real.  So everything the
 * new tree names must be durable BEFORE that write, and the write itself must
 * be durable before the commit can be called done -- hence two flushes, and
 * hence a failure of the second being the one outcome that is neither "it
 * happened" nor "it did not".
 *
 * Lives in core/ from phase 7: every step goes through the seam, so there is
 * no reason for two environments to hold two opinions about what a durable
 * commit is.
 */
int trans_commit(struct bitter_fs_info* fs, struct bitter_trans* trans,
      struct bitter_root* root);

#endif
