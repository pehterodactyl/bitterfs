#include "core/extent.h"
#include "core/fs.h"
#include "core/bitter_endian.h"
#include "core/bitter_assert.h"
#include "core/trans.h"


#define EXTENTS_START_OFFSET (BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE)

int extent_inc_ref(struct bitter_fs_info* fs_info, bt_u64 bytenr, bt_u64 length,
        struct bitter_trans* trans) {

  struct bitter_key_cpu key;
  bitter_extent_key(&key, bytenr, length);

  struct bitter_path path;
  bitter_path_init(&path);

  struct bitter_env* env = fs_info->env;

  /* By pointer, never a copy: the CoW descent rewrites bytenr and generation
   * when the extent tree's root relocates, and a copy would carry that update
   * out of scope at return. */
  struct bitter_root* extent_root = &fs_info->extent_root;
  
  /* ins_len 0: this descent only decides which case we are in.  The insert
   * path re-descends through btree_insert, which asks for its own room, so
   * splitting nodes here would be work thrown away.  cow 1 because the found
   * case writes into the leaf. */
  int s = btree_search(env, extent_root, &key, &path, 0, trans, 1);

  if (s < 0) {
    bitter_path_release(env, &path);
    return s;
  }

  /* No item yet -- this is the extent's first reference, and the normal case
   * for a freshly allocated block.  Not an error, unlike the same result in
   * bitter_find_root.
   *
   * Handed to btree_insert rather than inserted here.  btree_search's ins_len
   * only guarantees the NODES have slack; the leaf itself may still be full
   * (core/btree.c:232), and an insert into a full leaf writes past the free
   * space.  btree_insert owns that check, the split, the low-key fixup when
   * the item lands at slot 0, and the dirty_block -- all of it already
   * exercised by unit/split_leaf.  A second copy here is a second thing to
   * keep correct. */
  if (s != 0) {
    /* Every field written: the struct grew a `flags` member, and a partially
     * initialised payload would put stack garbage on disk. */
    struct bitter_extent_item ext;
    bt_put_le64(&ext.refs, 1);
    bt_put_le64(&ext.flags, 0);
    bitter_path_release(env, &path);
    return btree_insert(env, extent_root, &key, &path, &ext,
                        BITTER_EXTENT_ITEM_SIZE, trans);
  }

  /* The item exists: read, add one, write back.  No CoW needed -- the descent
   * copied every block in the path, so this leaf is already ours. */
  struct bitter_buf* buf = path.nodes[0];
  bt_u32 slot = (bt_u32)path.slots[0];
  void* bit_buf = buf->b_data;

  /* Validated before the write: 8 bytes put over a shorter item would run into
   * its neighbour's payload, and every descriptor would stay consistent
   * afterwards, so nothing downstream could detect it. */
  struct bitter_item* item = bitter_leaf_item(bit_buf, slot);
  if (bt_get_le32(&item->size) != BITTER_EXTENT_ITEM_SIZE) {
    bitter_path_release(env, &path);
    return -BITTER_EUCLEAN;
  }

  struct bitter_extent_item* ext = bitter_leaf_data(bit_buf, slot);
  bt_put_le64(&ext->refs, bt_get_le64(&ext->refs) + 1);

  env->ops->dirty_block(env, buf);
  bitter_path_release(env, &path);
  return 0;
}


void bitter_extent_key(struct bitter_key_cpu* key, bt_u64 bytenr, bt_u64 length) {
  key->objectid = bytenr;

  key->type = BITTER_EXTENT_ITEM;

  key->offset = length;

}

int extent_dec_ref(struct bitter_fs_info* fs_info, bt_u64 bytenr, bt_u64 length,
      struct bitter_trans* trans) {

  struct bitter_key_cpu key;
  bitter_extent_key(&key, bytenr, length);

  struct bitter_path path;
  bitter_path_init(&path);

  struct bitter_env* env = fs_info->env;

  /* BY POINTER.  A copy would take the CoW descent's update to bytenr and
   * generation with it when this function returns, and the filesystem would go
   * on using the extent tree's old, now-stale root. */
  struct bitter_root* extent_root = &fs_info->extent_root;

  int s = btree_search(env, extent_root, &key, &path, 0, trans, 1);

  if (s < 0) {
    bitter_path_release(env, &path);
    return s;
  }

  /* No item means somebody is releasing a reference that was never taken --
   * either a double free above us or a damaged tree, and from here the two are
   * indistinguishable.  A return rather than an assert for exactly that
   * reason: an assert would claim we know whose bug it is. */
  if (s != 0) {
    bitter_path_release(env, &path);
    return -BITTER_ENOENT;
  }

  struct bitter_buf* buf = path.nodes[0];
  void* bit_buf = buf->b_data;
  bt_u32 slot = (bt_u32)path.slots[0];

  struct bitter_item* it = bitter_leaf_item(bit_buf, slot);
  if (bt_get_le32(&it->size) != BITTER_EXTENT_ITEM_SIZE) {
    bitter_path_release(env, &path);
    return -BITTER_EUCLEAN;
  }

  struct bitter_extent_item* ext = bitter_leaf_data(bit_buf, slot);
  bt_u64 refs = bt_get_le64(&ext->refs);

  /* Branch on the value rather than decrement and then test: no arithmetic
   * happens before the guard, so a stored 0 can never wrap to (bt_u64)-1 --
   * which would leak the extent permanently, since it could never count back
   * down to zero again. */
  if (refs == 0) {
    /* Unreachable through any correct path: the last reference deletes the
     * item rather than storing a zero.  So this is a corrupt tree. */
    bitter_path_release(env, &path);
    return -BITTER_EUCLEAN;
  }

  if (refs == 1) {
    /* The last reference.  The item goes away entirely -- absence is what
     * means "free", which is what lets the mount scan read the gaps between
     * address-ordered items as the free space.
     *
     * btree_del_item runs its own descent and releases the path itself, so
     * this one is dropped first and nothing below may touch `buf`.  It also
     * owns the low-key fixup when the item was at slot 0. */
    bitter_path_release(env, &path);
    return btree_del_item(env, extent_root, &key, &path, trans);
  }

  bt_put_le64(&ext->refs, refs - 1);
  env->ops->dirty_block(env, buf);
  bitter_path_release(env, &path);
  return 0;
}

int extent_build_free_map(struct bitter_fs_info* fs) {

  struct bitter_env*  env  = fs->env;
  struct bitter_root* root = &fs->extent_root;

  /* Caller mistakes, not the disk's.  A zero capacity would turn every later
   * allocation into -BITTER_ENOSPC, which reads as a full disk. */
  BITTER_ASSERT(fs->free);
  BITTER_ASSERT(fs->free_cap > 0);

  /* Reset here rather than relying on the caller, so this is idempotent and
   * can be run again to rebuild the map. */
  fs->free_count = 0;

  struct bitter_path path;
  bitter_path_init(&path);

  /* (0, 0, 0) sorts below every real key -- objectid 0 is
   * BITTER_INVALID_OBJECTID and never names an item -- so this lands on slot 0
   * of the leftmost leaf and returns 1.  Only a negative return is a failure.
   *
   * ins_len 0, no transaction, no CoW: this function measures the tree and
   * must not be able to change it. */
  struct bitter_key_cpu key;
  key.objectid = 0;
  key.type     = 0;
  key.offset   = 0;

  int s = btree_search(env, root, &key, &path, 0, 0, 0);
  if (s < 0) {
    return s;                       /* search released the path */
  }

  /*
   * The accumulator: the end of the last allocated range seen so far, or the
   * start of allocatable space when none have been seen.  It is INITIALISED,
   * not read out of the tree, which is why the first item needs no special
   * case -- if it begins exactly here the gap is zero and is skipped.
   *
   * Starting at 0 assumes the extent tree describes the WHOLE device,
   * including the reserved prefix and the superblock.  mkfs does not record
   * those yet, so on a freshly formatted image this scan reports them as free.
   * See docs/LOG.md.
   */
  bt_u64 end = 0;

  for (;;) {

    /* Re-read every leaf: btree_next_leaf released the previous one. */
    void*  leaf    = path.nodes[0]->b_data;
    bt_u32 nritems = bitter_leaf_nritems(leaf);

    /* Zero iterations when the leaf is empty, which is a normal state -- an
     * emptied leaf stays in the tree, and extent_dec_ref creates them. */
    for (bt_u32 i = 0; i < nritems; i++) {

      struct bitter_item*   it = bitter_leaf_item(leaf, i);
      struct bitter_key_cpu k;
      bitter_key_from_disk(&k, &it->key);

      bt_u64 start = k.objectid;    /* the extent's address */
      bt_u64 len   = k.offset;      /* and its length: both live in the key */

      /*
       * This is the boundary where disk data becomes an in-memory structure,
       * so the validation belongs here -- and it is what earns the assertions
       * bitter_alloc_block makes about the map afterwards.
       *
       * `start < end` means two allocated ranges overlap.  Only detectable
       * here, because it is a relationship between adjacent items rather than
       * a property of either one.
       */
      if (k.type != BITTER_EXTENT_ITEM ||
          len == 0 ||
          start % BITTER_BLOCK_SIZE != 0 ||
          len   % BITTER_BLOCK_SIZE != 0 ||
          start < end) {
        bitter_path_release(env, &path);
        return -BITTER_EUCLEAN;
      }

      if (start > end) {
        /* Checked BEFORE the write: afterwards would already have stored at
         * free[free_cap], one past the end of the caller's array. */
        if (fs->free_count == fs->free_cap) {
          bitter_path_release(env, &path);
          return -BITTER_ENOMEM;    /* the array is full, not the device */
        }
        fs->free[fs->free_count].start  = end;
        fs->free[fs->free_count].length = start - end;
        fs->free_count++;
      }
      /* Adjacent extents give start == end and no entry at all, which is the
       * common case: a zero-length range would trip bitter_alloc_block's
       * assert and waste a slot in an array that cannot grow. */

      end = start + len;
    }

    int n = btree_next_leaf(env, root, &path);
    if (n < 0) {
      return n;                     /* released by next_leaf */
    }
    if (n == 1) {
      break;                        /* released by next_leaf */
    }
  }

  /* The one gap no item produces, handled after the loop rather than inside
   * it.  On an image with no extent items this is the only entry emitted, and
   * it covers the whole device. */
  if (fs->total_bytes > end) {
    if (fs->free_count == fs->free_cap) {
      return -BITTER_ENOMEM;
    }
    fs->free[fs->free_count].start  = end;
    fs->free[fs->free_count].length = fs->total_bytes - end;
    fs->free_count++;
  }

  return 0;
}



int extent_apply_delayed_refs(struct bitter_fs_info* fs, struct bitter_trans* trans) {

  /*
   * A bound rather than a plain loop.  Termination is an argument -- each
   * round copy-on-writes fewer blocks than the last, so it converges -- and an
   * argument that turns out to be wrong should be a diagnosable failure rather
   * than a hang inside a commit.
   */
  bt_u32 rounds = 0;

  while (trans->ref_count > 0) {

    BITTER_ASSERT(rounds < BITTER_MAX_DRAIN_ROUNDS);
    rounds++;

    /*
     * Taken OFF the set before it is applied, not after.
     *
     * Applying is re-entrant: it copy-on-writes the extent tree, which
     * allocates, which appends new entries here -- and once btree_cow_block
     * records a -1 for each block it abandons, a merge that nets to zero will
     * also REMOVE entries, swapping the last into the hole.  Either can move
     * whatever sits at index 0 while this call is running.
     *
     * Copying the entry out and dropping it first makes all of that
     * irrelevant: the set never holds a half-applied entry, and nothing the
     * nested calls do to the array can touch what we are working on.
     */
    struct bitter_delayed_ref ref = trans->refs[0];
    trans->refs[0] = trans->refs[trans->ref_count - 1];
    trans->ref_count--;

    bt_u64 bytenr = ref.bytenr;
    bt_u64 length = ref.length;
    int    delta  = ref.delta;

    /* delta is never 0: trans_add_delayed_ref removes an entry the moment a
     * merge cancels it out, so a zero never reaches the set. */
    if (delta < 0) {
      delta = -delta;
      for (int i = 0; i < delta; i++) {
        /* |delta| separate descents rather than one call taking a delta.
         * Deliberate: |delta| is 1 in almost every case -- a surviving 2 needs
         * one extent referenced twice in a single transaction, which is
         * snapshots at phase 6 -- so the extra descents cost nothing and
         * inc/dec stay two small, separately tested functions.  A combined
         * extent_change_ref is the escape if that stops being true.
         *
         * Every iteration is checked, not just the last: a failure on the
         * second of three leaves the extent tree holding a partial delta with
         * the entry already off the set, which is the half-applied state this
         * function's contract warns about. */
        int s = extent_dec_ref(fs, bytenr, length, trans);
        if (s < 0) {
          return s;
        }
      }
    }
    else {
      for (int i = 0; i < delta; i++) {
        int s = extent_inc_ref(fs, bytenr, length, trans);
        if (s < 0) {
          return s;
        }
      }
    }
  }

  return 0;
}
