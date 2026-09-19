/*
 * The transaction's delayed reference set.
 *
 * Every allocation and every free during a transaction is recorded here, in
 * memory, and applied to the extent tree in one pass at commit.  Applying them
 * immediately is not possible: recording a reference modifies the extent tree,
 * which copy-on-writes its blocks, which allocates, which needs another
 * reference recorded.  Deferring breaks that cycle, and it is safe because the
 * in-memory free map has ALREADY been updated -- the block is out of the free
 * list the moment it is handed out, so nothing else can be given the same
 * address before the set is drained.
 */
#include "core/trans.h"
#include "core/bitter_assert.h"
#include "core/bitter_string.h"
#include "core/bitter_endian.h"
#include "core/bitter_crc32c.h"
#include "core/root.h"
#include "core/extent.h"

int trans_add_delayed_ref(struct bitter_trans* trans, bt_u64 bytenr, bt_u64 length,
      int delta) {

  /* Both ours, not the disk's: an overrun array is a bug in this file, and a
   * delta of zero can only come from a caller that computed a no-op and did
   * not notice. */
  BITTER_ASSERT(trans->ref_count <= trans->ref_cap);
  BITTER_ASSERT(delta != 0);

  bt_u32 ref_count = trans->ref_count;
  bt_u32 ref_cap   = trans->ref_cap;

  /*
   * Exact match on both fields.  An extent item is keyed (bytenr, EXTENT_ITEM,
   * length), so [0x1000, 0x2000) and [0x0, 0x11000) are two different items
   * even though one range contains the other -- treating containment as a
   * match would apply a delta to the wrong item, or to one that does not
   * exist.  The invariant that makes this safe is that a free names exactly
   * the extent that was allocated.
   *
   * A miss is the ordinary case: the set starts empty at every transaction, so
   * a first touch has nothing to merge with.  A hit means the same extent was
   * touched twice -- allocated then freed, or referenced twice by a snapshot.
   */
  for (bt_u32 i = 0; i < ref_count; i++) {
    struct bitter_delayed_ref* ref = &trans->refs[i];
    if (ref->bytenr == bytenr && ref->length == length) {
      ref->delta += delta;

      /* Netted out -- allocated and freed inside one transaction.  Dropping
       * the entry rather than storing a zero is the point of merging: the set
       * stays proportional to what the transaction NET did, and the drain
       * never has to skip no-ops or create an item it would delete again.
       *
       * Order does not matter here, so the last entry fills the hole. */
      if (ref->delta == 0) {
        trans->refs[i] = trans->refs[ref_count - 1];
        trans->ref_count--;
      }
      return 0;
    }
  }

  /*
   * Checked before the write: afterwards would already have stored at
   * refs[ref_cap], past the end of the caller's array.
   *
   * Unlike every other -BITTER_ENOMEM in the filesystem, this one is NOT soft.
   * A full free map can be rebuilt at the next mount, so losing a range only
   * leaks space; an allocation that is never recorded is a block handed out
   * twice.  A caller that sees this must abort the transaction.
   */
  if (ref_count == ref_cap) {
    return -BITTER_ENOMEM;
  }

  trans->refs[ref_count].bytenr = bytenr;
  trans->refs[ref_count].length = length;
  trans->refs[ref_count].delta  = delta;
  trans->ref_count = ref_count + 1;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Moved here from user/trans.c at phase 7.                            */
/*                                                                     */
/* Every step of a commit already went through the seam EXCEPT the     */
/* superblock read and write, which is the single reason this had to   */
/* live in an environment.  read_super and write_super close that gap. */
/*                                                                     */
/* The commit ordering is the one thing in the filesystem whose        */
/* failure is invisible until a power cut, so it gets exactly one      */
/* definition rather than one per environment.                         */
/* ------------------------------------------------------------------ */

int bitter_read_super_raw(struct bitter_env* env, struct bitter_super* sb) {

  /* Through the seam, so this works identically in the kernel.  The
   * environment moves BITTER_SUPER_SIZE bytes and validates nothing: what a
   * valid superblock IS belongs here, not in an environment. */
  int rr = env->ops->read_super(env, sb);
  if (rr < 0) {
    return rr;
  }

  
  
  int r = memcmp(sb->magic,BITTER_MAGIC, BITTER_MAGIC_SIZE);
  if (r != 0) {
    return -BITTER_EINVAL;
  }
  
  if (bt_get_le32(sb->csum) != bt_block_csum(sb, BITTER_SUPER_SIZE)) {
    return -BITTER_EUCLEAN;
  }

  if (bt_get_le64(&sb->bytenr) != BITTER_SUPER_OFFSET) {
    return -BITTER_EUCLEAN;
  }
 
  if (sb->root_level >= BITTER_MAX_LEVEL) {
    return -BITTER_EUCLEAN;
  }

  return 0; 

}

int bitter_read_super(struct bitter_env* env, struct bitter_root* root) {
  
  /* No memset: the helper fills every byte or fails, and zeroing first would
   * hide a partial read behind plausible-looking fields. */
  struct bitter_super sb;
  int r = bitter_read_super_raw(env, &sb);
  if (r < 0) {
    return r;
  }

  /* Nothing above this line touched `root`, so a refusal leaves the caller's
   * handle exactly as it was. */
  root->level       = sb.root_level;
  root->generation  = bt_get_le64(&sb.generation);
  root->bytenr      = bt_get_le64(&sb.root);
  root->total_bytes = bt_get_le64(&sb.total_bytes);
  /* The allocator's high-water mark, which lives in bytes_used -- see
   * docs/LOG.md.  Without it a reopened image allocates over live blocks. */
  root->next_free   = bt_get_le64(&sb.bytes_used);

  /* Not on disk: this tree is the root tree by definition. */
  root->objectid    = BITTER_ROOT_TREE_OBJECTID;

  return 0;
}

int trans_start(struct bitter_env* env, struct bitter_trans* trans) {
  struct bitter_super sb;
  int f = bitter_read_super_raw(env, &sb);
  if (f < 0) {
    return f;
  }
  bt_u64 generation = bt_get_le64(&sb.generation);
  trans->generation = generation + 1;
  return 0;
}

int trans_commit(struct bitter_fs_info* fs, struct bitter_trans* trans,
      struct bitter_root* root) {

  struct bitter_env* env = fs->env;

  /*
   * Everything this transaction allocated is still only a promise in
   * trans->refs.  Applying it has to happen before writeback, because it
   * dirties extent-tree blocks, and before the superblock write, because the
   * extent tree's root moves while it runs.
   *
   * The loop is the awkward part.  Draining the set copy-on-writes the extent
   * tree, so its root moves and bitter_update_root has to record the new
   * address -- but recording it copy-on-writes the ROOT tree, which allocates,
   * which fills the set again.  It converges because copy-on-write is
   * idempotent within a transaction: once a block carries this generation it
   * is modified in place and allocates nothing, so each pass touches fewer
   * blocks than the last and eventually one adds nothing at all.
   *
   * `root` rather than fs->tree_root, and non-const now: the caller names the
   * root tree being committed, bitter_update_root searches it, and its own
   * root moves in the process.
   */
  /*
   * A zero bytenr means this filesystem has no extent tree.  No image mkfs
   * writes can be in that state -- it always creates one -- but the unit
   * fixtures are: they build a root tree by hand and never make a second.
   * With nowhere to record allocations, the pending set is discarded rather
   * than applied to a tree that is not there.
   */
  if (fs->extent_root.bytenr == 0) {
    trans->ref_count = 0;
  }

  bt_u32 passes = 0;
  while (fs->extent_root.bytenr != 0) {
    int r = extent_apply_delayed_refs(fs, trans);
    if (r < 0) {
      return r;
    }

    /*
     * EVERY tree whose root may have moved, not just the extent tree.
     *
     * Phase 4 wrote this loop when the extent tree was the only second tree,
     * and it stayed correct for as long as nothing else was ever modified.
     * Phase 7 breaks that: a write copy-on-writes the FS tree, so its root
     * moves too -- and a root that moves without being recorded is not an
     * error.  The OLD tree is still perfectly valid, because that is what
     * copy-on-write guarantees.  So the commit succeeds, the filesystem mounts
     * cleanly, fsck reports nothing, and the change is simply gone.
     *
     * Silent loss rather than corruption is the worst shape a bug can take
     * here, which is why both roots are recorded and both are watched below.
     */
    bt_u64 ext_before = fs->extent_root.bytenr;
    bt_u64 fs_before  = fs->fs_root.bytenr;

    r = bitter_update_root(env, root, &fs->extent_root, trans);
    if (r < 0) {
      return r;
    }

    /*
     * Skipped when absent, for the same reason the extent tree's guard exists:
     * the unit fixtures build a root tree by hand and never create an FS tree.
     * A zero bytenr is "this filesystem has no such tree", never a tree at
     * address zero -- that is the reserved region.
     */
    if (fs->fs_root.bytenr != 0) {
      r = bitter_update_root(env, root, &fs->fs_root, trans);
      if (r < 0) {
        return r;
      }
    }

    /* Settled: the drain added nothing and NEITHER root moved, so what the
     * root tree records is what the trees are.  Either one moving means the
     * recording allocated, which means another round. */
    if (trans->ref_count == 0 &&
        fs->extent_root.bytenr == ext_before &&
        fs->fs_root.bytenr == fs_before) {
      break;
    }

    /* A bound rather than a bare loop, for the same reason the drain has one:
     * a convergence argument that turns out to be wrong should fail where it
     * can be read, not hang inside a commit. */
    passes++;
    if (passes >= BITTER_MAX_DRAIN_ROUNDS) {
      return -BITTER_EUCLEAN;
    }
  }

  int f = env->ops->writeback_all(env);
  if (f < 0) {
    return f;
  }
  f = env->ops->flush_device(env);
  if (f < 0) {
    return f;
  } 
 
  struct bitter_super sb;
  f = bitter_read_super_raw(env, &sb);
  if (f < 0) {
    return f;
  }
  sb.root_level = root->level;
  
  bt_put_le64(&sb.root, root->bytenr);
  /* trans, not root: super.generation is the last COMMITTED transaction, while
   * root->generation is the root block's own and is what btree_search
   * cross-checks.  Equal today; they mean different things. */
  bt_put_le64(&sb.generation, trans->generation);
  bt_put_le64(&sb.bytes_used, root->next_free);

  /* Last, after every field above: the checksum covers everything that follows
   * it, so computing it earlier would certify the old values. */
  bt_put_le32(sb.csum, bt_block_csum(&sb, BITTER_SUPER_SIZE));

  /* THE write.  Everything the new tree names was made durable by the flush
   * above; this single overwrite is the moment the transaction becomes real,
   * which is why the superblock is the one structure never copy-on-written. */
  f = env->ops->write_super(env, &sb);
  if (f < 0) {
    return f;
  }

  /* The second barrier, and the moment the transaction is over.  A failure here
   * is the one outcome that is neither "it happened" nor "it did not": the
   * superblock reached the page cache, and whether it survives a power cut is
   * unknown.  A blind retry commits twice. */
  f = env->ops->flush_device(env);
  if (f < 0) {
    return f;
  }

  return 0;
}
