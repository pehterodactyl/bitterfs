#include "btree.h"
#include "core/bitter_string.h"
#include "core/bitter_assert.h"
#include "core/alloc.h"
#include "core/cow.h"

/* Forward declared so the public entry points stay at the top of the file. */
static void fixup_low_keys(struct bitter_env* env, struct bitter_path* path,
        const struct bitter_key_cpu* key, bt_u8 level);






void bitter_path_init(struct bitter_path* path) {
  for (bt_u32 i = 0; i < BITTER_MAX_LEVEL; i++) {
    path->nodes[i] = 0;
    path->slots[i] = 0;
  }
}

void bitter_path_release(struct bitter_env* env, struct bitter_path* path) {
  for (bt_u32 i = 0; i < BITTER_MAX_LEVEL; i++) {
    if (path->nodes[i]) {
      env->ops->put_block(env, path->nodes[i]);
      /* Cleared, not just released: that is what makes a second call safe and
       * lets a caller release unconditionally on the way out. */
      path->nodes[i] = 0;
    }
    path->slots[i] = 0;
  }
}


/*
 * Free slots a node must keep during an insert descent.  Two, because a leaf
 * split can add two entries to its parent (btree_split_leaf's MIDDLE shape).
 * A node split adds only one, so above level 1 this is a slot conservative --
 * one of 121, which is not worth a per-level rule.
 */
#define BITTER_INSERT_SLACK 2

/*
 * Descends from the root to the leaf that would contain `key`, filling `path`
 * with the buffer and slot at every level.
 *
 * Returns 0 found, 1 NOT FOUND, negative errno.  Not-found is not a failure:
 * insert needs the slot where the key would go, and path->slots[0] is exactly
 * that.
 *
 * `path` must be zeroed or previously released -- see bitter_path_init.
 * On any error return the path is released, so the caller frees nothing.
 *
 * `ins_len` is the size of an item the caller intends to insert, or 0 for a
 * pure lookup.  Non-zero means the descent must leave every node it passes
 * through with room to absorb a split from below.  Only its zero-ness is read
 * today; it is a length rather than a flag because btrfs's equivalent is, and
 * because moving btree_split_leaf into the descent would need the value.
 *
 * Takes the root by pointer, not by value: a split at the top rewrites
 * root->bytenr and root->level mid-descent, and every split needs
 * root->next_free.
 *
 * `trans` is the open transaction, and may be null for a pure lookup -- which
 * is the whole point of carrying it: a caller with no transaction cannot reach
 * any code that writes.  A splitting descent allocates, and an allocation has
 * to stamp a generation, so the two are checked against each other below.
 */
int btree_search(struct bitter_env *env, struct bitter_root *root,
        const struct bitter_key_cpu *key, struct bitter_path *path,
        bt_u32 ins_len, struct bitter_trans *trans, bt_u8 cow) {

  /* Ours, not the caller's: every in-tree caller that passes ins_len has a
   * transaction in hand already. */
  BITTER_ASSERT(!ins_len || cow);
  BITTER_ASSERT(!cow || trans);

  bitter_path_release(env, path);

  /* From the superblock, so disk data: an error, not an assert.  Checked
   * before the loop because it is what makes path->nodes[level] in range for
   * every iteration -- level only decreases from here. */
  if (root->level >= BITTER_MAX_LEVEL) {
    return -BITTER_EUCLEAN;
  }

  /* Snapshots, deliberately.  `level` tracks the block this loop is standing
   * on, which does not move when a split above inserts a new root -- so it
   * must not follow root->level.  root->bytenr is only read once either way. */
  bt_u64 bytenr = root->bytenr;
  bt_u8 level = root->level;
  bt_u64 generation = root->generation;

  for (;;) {
    struct bitter_buf* buf = env->ops->read_block(env, bytenr);
    if (!buf) {
      /* Nothing to release for THIS level, but earlier levels are in the
       * path and would leak without this. */
      bitter_path_release(env, path);
      return -BITTER_EIO;
    }

    /* Recorded BEFORE any check that can fail.  Between read_block and this
     * line the buffer is owned by nobody, so every failure below can simply
     * release the path. */
    path->nodes[level] = buf;

    const struct bitter_header* hdr = buf->b_data;

    /* The one check the structural checkers cannot make: a leaf and a node are
     * both valid blocks, and only the PARENT knows which it pointed at.
     * Without this, a leaf's 25-byte items get read as 33-byte key_ptrs. */
    if (hdr->level != level) {
      bitter_path_release(env, path);
      return -BITTER_EUCLEAN;
    }

    if (bt_get_le64(&hdr->generation) != generation) {
      bitter_path_release(env, path);
      return -BITTER_EUCLEAN;
    }

    /* Structure only, in phase 2.  The self-identifying checks -- checksum,
     * fsid, and bytenr matching where we read it from -- arrive in phase 3,
     * when blocks start genuinely round-tripping through a disk.  Requiring
     * them now would mean every test computed a checksum for every block it
     * fabricates. */
    bt_u32 bad = level ? bitter_node_check(buf->b_data)
                       : bitter_leaf_check(buf->b_data);
    if (bad) {
      bitter_path_release(env, path);
      return -BITTER_EUCLEAN;
    }

    if (cow) {
      int f = btree_cow_block(env, root, path, level, trans);
      if (f < 0) {
        bitter_path_release(env, path);
        return f;
      }
      buf = path->nodes[level];
      hdr = buf->b_data;
    }

    int found = 0;
    bt_u32 stride = (bt_u32)(level ? sizeof(struct bitter_key_ptr)
                                   : sizeof(struct bitter_item));
    bt_u32 slot = bitter_bsearch((bt_u8*)buf->b_data + BITTER_HEADER_SIZE, stride,
            bitter_leaf_nritems(buf->b_data), key, &found);

    if (level == 0) {
      path->slots[0] = slot;
      return found ? 0 : 1;
    }

    /* The descent adjustment.  key_ptr[i].key is the SMALLEST key in child i's
     * subtree, and bsearch returned the first entry GREATER than our key -- so
     * the child that could contain it is the one before.
     *
     * Only when not found: an exact match means our key is the smallest in
     * that subtree, so that child is already right.  And only when slot > 0:
     * at 0 every key here exceeds ours, and child 0 is where such a key would
     * live.  Without the clamp, slot - 1 underflows to ~4 billion. */
    if (!found && slot > 0) {
      slot--;
    }
    path->slots[level] = slot;

    /*
     * Top-down splitting.  A node is given room BEFORE the descent commits to
     * one of its children, never after: once we drop a level the choice is
     * made, and a split down there would find this node full with nowhere to
     * put the separator.
     *
     * Placed after path->slots[level] is stored because btree_split_node reads
     * that slot to decide which half the descent continues into -- and
     * rewrites both it and path->nodes[level] when the answer is the sibling.
     *
     * Why one slot is always available to the split: at the root, split_node
     * grows a new one and inserts into that.  Below the root, the previous
     * iteration ran this same check on the parent, so it holds at least
     * BITTER_INSERT_SLACK -- or was itself split, leaving both halves near
     * half empty.
     */
    if (ins_len && bitter_node_free_slots(buf->b_data) < BITTER_INSERT_SLACK) {
      int sr = btree_split_node(env, path, level, root, trans);
      if (sr < 0) {
        bitter_path_release(env, path);
        return sr;
      }
      /* buf and slot are both stale now: the block we were standing on may
       * have been released and replaced by its sibling. */
      buf  = path->nodes[level];
      slot = (bt_u32)path->slots[level];
    }

    bytenr = bitter_node_blockptr(buf->b_data, slot);
    generation = bitter_node_generation(buf->b_data, slot);    

    level--;
  }
  /* No return here: the loop never exits.  A trailing one would be dead code
   * that reads as a real path. */
}


int btree_insert(struct bitter_env *env, struct bitter_root* root, 
      const struct bitter_key_cpu* key, struct bitter_path* path, const void* data, bt_u32 size,
      struct bitter_trans* trans) {
  const bt_u32 ITEM = (bt_u32)sizeof(struct bitter_item);

  /* `size` comes from the caller, so this is an error return where
   * btree_split_leaf makes the same test an assert.  Before the search, not
   * after: a search with ins_len set splits nodes on the way down, and that is
   * real work -- and real tree mutation -- for a request about to be refused. */
  if (size > BITTER_MAX_ITEM_SIZE) {
    return -BITTER_EINVAL;
  }

  int r = btree_search(env, root, key, path, size + ITEM, trans, 1);
 //Received an error from search, we pass that error through
  if (r < 0) {
    return r;
  }
  
  if (r == 0) {
    bitter_path_release(env, path);
    return -BITTER_EEXIST;
  }

  /* The descent guaranteed the parent has room for the separators a leaf split
   * adds; all that can be missing now is room in the leaf itself. */
  if (bitter_leaf_free_space(path->nodes[0]->b_data) < size + ITEM) {
    int sr = btree_split_leaf(env, path, root, key, size, trans);
    if (sr < 0) {
      bitter_path_release(env, path);
      return sr;
    }
  }

  /* Read AFTER the split, never before: btree_split_leaf may have moved
   * path->nodes[0] onto a sibling and rewritten path->slots[0]. */
  void* leaf = path->nodes[0]->b_data;

  /* Ours to get right, not the disk's or the caller's: split_leaf's fallback
   * shapes give the item a leaf of its own, and the largest legal item fits an
   * empty leaf exactly.  So a second split is never needed. */
  BITTER_ASSERT(bitter_leaf_free_space(leaf) >= size + ITEM);

  bitter_leaf_insert(leaf, key, path->slots[0], data, size);
  env->ops->dirty_block(env, path->nodes[0]);
  if (path->slots[0] == 0) {
    fixup_low_keys(env, path, key, 1);
  }  
  bitter_path_release(env, path);
  return 0;
 
}


static void fixup_low_keys(struct bitter_env* env, struct bitter_path* path, 
        const struct bitter_key_cpu* key, bt_u8 level) {

  BITTER_ASSERT(level >= 1 && level < BITTER_MAX_LEVEL);
  for (; level < BITTER_MAX_LEVEL; level++) {
    
    if (!path->nodes[level]) {
        break;
    }
    
    struct bitter_key_ptr* ptr = bitter_node_key_ptr(path->nodes[level]->b_data, 
              path->slots[level]);
    
    bitter_key_to_disk((struct bitter_key*)ptr, key); 
    env->ops->dirty_block(env, path->nodes[level]);
    
    if (path->slots[level] != 0) {
      break;
    } 
  }  
}

/*
 * Splits the node at path->nodes[level], moving the upper half of its entries
 * into a fresh sibling and giving the parent an entry for it.
 *
 * The UPPER half moves, not the lower, and that is what keeps this simple: the
 * original block's first key does not change, so its existing separator in the
 * parent stays correct and no fixup_low_keys is needed.  Moving the lower half
 * would change both blocks' first keys and require repairing the parent twice.
 *
 * Returns 0, -BITTER_ENOSPC (no device space, or the tree is already at
 * BITTER_MAX_LEVEL) or -BITTER_ENOMEM.  Never -BITTER_EIO: nothing is read
 * here, so no bytes can fail to arrive.
 *
 * No split-point decision, unlike a leaf: key_ptrs are all 33 bytes, so the
 * count midpoint is also the byte midpoint.
 */
int btree_split_node(struct bitter_env* env, struct bitter_path* path, 
    bt_u8 level, struct bitter_root* root, struct bitter_trans* trans) {


  struct bitter_buf* node = path->nodes[level];
  struct bitter_header* hdr = node->b_data;

  /*
   * The root case, handled FIRST so that the "insert into the parent" step
   * below is uniform -- by the time it runs, a parent always exists.
   *
   * Splitting first and then discovering there is no parent would mean two
   * code paths for the same insert, and a half-split tree if the second
   * allocation failed.
   */
  if (level == root->level) {

    /*
     * A capacity limit, not corruption: the filesystem genuinely cannot hold
     * another item.  So an error rather than an assert -- an assert would
     * abort the kernel on a full filesystem.
     *
     * Note btree_search rejects the same comparison as -BITTER_EUCLEAN,
     * because there the level came off the disk and a tree claiming
     * impossible depth is corrupt.  Same constant, opposite meaning.
     *
     * Checked before either allocation: a bump allocator never reclaims, so
     * allocating and then failing would lose those blocks permanently.
     */
    if (root->level + 1 >= BITTER_MAX_LEVEL) {
      return -BITTER_ENOSPC;
    }

    struct bitter_buf* new_root_node;
    int ar_new_root_node = bitter_alloc_block(env, root, level+1, trans, &new_root_node);
    
    if (ar_new_root_node < 0) {
      return ar_new_root_node;
    }
    
    /* The only operation in the whole tree that changes any of these three.
     * All must move together: btree_search starts both its level and its
     * generation expectation from root, so updating one without the others
     * fails on the first read.
     *
     * At phase 3 this is also where the change has to reach super.root and
     * super.root_level.  Nothing persists them yet -- reopening an image would
     * find the old root -- which is a gap the commit protocol closes. */
    struct bitter_header* root_hdr = (struct bitter_header*)new_root_node->b_data;
    root->bytenr = bt_get_le64(&root_hdr->bytenr);
    root->level++;
    root->generation = bt_get_le64(&root_hdr->generation);

    struct bitter_key_cpu old_root_key_cpu;
    bt_u64 hdr_blockptr = bt_get_le64(&hdr->bytenr);
    bt_u64 hdr_generation = bt_get_le64(&hdr->generation);
    
    bitter_key_from_disk(&old_root_key_cpu,(struct bitter_key*)((bt_u8*)hdr + BITTER_HEADER_SIZE));
    bitter_node_insert(new_root_node->b_data, 0, &old_root_key_cpu,
                       hdr_blockptr, hdr_generation);
    path->nodes[level+1] = new_root_node; 
    path->slots[level+1] = 0;
    env->ops->dirty_block(env, new_root_node);
  }

  struct bitter_buf* new_node;
    int ar_new_node = bitter_alloc_block(env, root, level, trans, &new_node);
  
  if (ar_new_node < 0) {
      return ar_new_node;
    }
  
  /* The original keeps the ceiling half, the sibling takes the floor.  Which
   * way round matters only for an odd count, and either is fine as long as
   * both halves shrink -- otherwise a split can produce a block that is still
   * full, which loops rather than merely being wrong. */
  bt_u32 nritems = bt_get_le32(&hdr->nritems);
  bt_u32 new_items =  nritems / 2;
  nritems -= new_items;
  struct bitter_header* new_hdr = new_node->b_data;
  bt_u8* start = (bt_u8*)hdr + BITTER_HEADER_SIZE + nritems * sizeof(struct bitter_key_ptr);
  memmove((bt_u8*)new_hdr + BITTER_HEADER_SIZE, start,  new_items * sizeof(struct bitter_key_ptr));
  
  bt_put_le32(&hdr->nritems, nritems);
  bt_put_le32(&new_hdr->nritems, new_items);

  /*
   * The parent is already in the path -- btree_search filled it during the
   * descent, or the root case above put the new root there.
   *
   * slot + 1 because the sibling holds the LARGER keys, so it sorts
   * immediately after the block it was split from.  Without this insert the
   * sibling is unreachable: a search reaches the parent, finds only the
   * original's entry, and everything moved across is on disk, correct, and
   * invisible to the tree.
   */
  struct bitter_buf* parent = path->nodes[level+1];
  bt_u32 slot = path->slots[level+1] + 1;
  struct bitter_key* new_key = (struct bitter_key*)((bt_u8*)new_hdr + BITTER_HEADER_SIZE);
  struct bitter_key_cpu new_key_cpu;
  bitter_key_from_disk(&new_key_cpu, new_key);
     
  bt_u64 new_hdr_blockptr = bt_get_le64(&new_hdr->bytenr);
  bt_u64 new_hdr_gen = bt_get_le64(&new_hdr->generation);
  bitter_node_insert(parent->b_data, slot, &new_key_cpu, new_hdr_blockptr, new_hdr_gen);

  /*
   * Move the path onto whichever half the descent was heading for.
   *
   * THE STEP THAT SILENTLY BREAKS A DESCENT.  This runs mid-search, and the
   * loop continues from path->nodes[level] afterwards -- so if the entry the
   * search selected has moved into the sibling, the path has to follow it.
   * Otherwise the descent carries on through the half that no longer holds
   * the key and reports not-found for something that exists.
   *
   * `nritems` is now the count LEFT in the original, so a slot at or beyond it
   * belongs to the sibling, at slot - nritems.  The parent slot advances by
   * one because the sibling's entry was just inserted there.
   *
   * Assumes path->slots[level] already names the entry the descent chose,
   * which is true when split runs after the binary search at this level.
   */
  if (path->slots[level] >= (int)nritems) {
    path->slots[level] -= (int)nritems;
    path->slots[level + 1] += 1;
    /*
     * The original leaves the path, so it is ours to release; the sibling
     * takes its place and becomes the path's.
     *
     * BOTH must be dirtied before that swap.  The sibling is about to be owned
     * by the path, and bitter_path_release only calls put_block -- which
     * writes back nothing unless the flag is already set.  Forgetting it here
     * leaves the sibling on disk as the empty block the allocator produced.
     */
    env->ops->dirty_block(env, node);
    env->ops->dirty_block(env, new_node);
    env->ops->put_block(env, node);
    path->nodes[level] = new_node;
    new_node = 0;
  }

  env->ops->dirty_block(env, parent);

  /* new_node is zero when the block above handed it to the path, in which case
   * `node` was released there and both are already dirty. */
  if (new_node) {
    env->ops->dirty_block(env, node);
    env->ops->dirty_block(env, new_node);
    /* The only buffer this function owns outright: it is in no path, so it
     * must reach put_block here.  Dirty first -- put_block writes back only
     * when the flag is set. */
    env->ops->put_block(env, new_node);
  }

  return 0;

}

int btree_del_item(struct bitter_env* env, struct bitter_root* root,
    const struct bitter_key_cpu* key, struct bitter_path* path,
    struct bitter_trans* trans) {
  
  int r = btree_search(env, root, key, path, 0, trans, 1);
  //Similar idea to insert, if search has problems pass that error through
  if (r < 0) {
    return r;
  }
  //if not found return error, different from insert which returns if found 
  if (r > 0) {
    bitter_path_release(env, path);
    return -BITTER_ENOENT;
  }

  struct bitter_buf* buf = path->nodes[0];
  int slot = path->slots[0];
  bt_u32 nritems = bitter_leaf_nritems(buf->b_data);
   
  bitter_leaf_remove(buf->b_data, (bt_u32)slot);
  
  env->ops->dirty_block(env, path->nodes[0]);
  
  if (nritems > 1 && slot == 0) {
    struct bitter_item* new_item = bitter_leaf_item(buf->b_data, 0);
    struct bitter_key_cpu new_key;
    bitter_key_from_disk(&new_key, &new_item->key);
    fixup_low_keys(env, path, &new_key, 1);
  }
  
  bitter_path_release(env, path);

  return 0;  

}


/*
 * Splits the leaf at path->nodes[0] so that an item of `size` bytes can be
 * inserted at path->slots[0].
 *
 * Does NOT insert the item.  It makes room and leaves path->nodes[0] and
 * path->slots[0] pointing at where the item belongs; btree_insert then calls
 * bitter_leaf_insert, which stays the only place an item is written.
 *
 * `key` is needed even so: in the fallback shapes a brand new leaf is created
 * for the item alone, and its entry in the parent needs a separator -- which
 * an empty leaf cannot supply.
 *
 * Four shapes, deliberately kept apart rather than folded together:
 *
 *   midpoint   the normal case.  Split where the leaf balances; the item lands
 *              inside one half.
 *   append     slot == nritems and the item will not fit.  A new leaf holding
 *              only the item goes AFTER the original; nothing moves.
 *   prepend    slot == 0 and it will not fit.  A new leaf goes BEFORE the
 *              original; nothing moves, but the parent's first key changes.
 *   middle     everything else that will not fit.  Two new leaves: one for the
 *              item, one for [slot, nritems).
 *
 * The fallbacks cannot fail to fit, because BITTER_MAX_ITEM_SIZE plus one
 * descriptor is exactly BITTER_LEAF_DATA_SIZE -- the largest legal item fits an
 * empty leaf precisely.  So split is called at most once per insert.
 */
int btree_split_leaf(struct bitter_env *env, struct bitter_path *path, struct bitter_root *root, 
       const struct bitter_key_cpu *key, bt_u32 size, struct bitter_trans *trans) {

  const bt_u32 ITEM = (bt_u32)sizeof(struct bitter_item);
  struct bitter_buf* leaf = path->nodes[0];
  void* buf = leaf->b_data;

  bt_u32 slot    = (bt_u32)path->slots[0];
  bt_u32 nritems = bitter_leaf_nritems(buf);
  bt_u32 need    = size + ITEM;      /* payload AND descriptor */

  /*
   * A leaf holding a single BITTER_MAX_ITEM_SIZE item is full, and there is no
   * midpoint that leaves both halves non-empty -- bitter_leaf_split_point's clamp returns
   * 0 for it.  Guard the call rather than the clamp: with one item the only
   * legal slots are 0 and nritems, so a fallback shape always applies.
   */
  bt_u32 mid = (nritems >= 2) ? bitter_leaf_split_point(buf) : 0;

  BITTER_ASSERT(nritems >= 1);                 /* an empty leaf cannot be full */
  BITTER_ASSERT(nritems < 2 || (mid >= 1 && mid < nritems));
  BITTER_ASSERT(slot <= nritems);              /* == nritems means append */
  BITTER_ASSERT(size <= BITTER_MAX_ITEM_SIZE); /* btree_insert rejects larger */

  /*
   * The root case first, so everything below can assume a parent exists.
   * Identical to bitter_split_node's, except the leaf is level 0 so the new
   * root is level 1.
   */
  if (root->level == 0) {

    if (root->level + 1 >= BITTER_MAX_LEVEL) {
      return -BITTER_ENOSPC;
    }

    struct bitter_buf* new_root_node;
    int ar_new_root_node = bitter_alloc_block(env, root, 1, trans, &new_root_node);
    if (ar_new_root_node < 0) {
      return ar_new_root_node;
    }

    struct bitter_header* leaf_hdr = buf;
    struct bitter_key_cpu old_root_key;
    bitter_key_from_disk(&old_root_key, &bitter_leaf_item(buf, 0)->key);

    /* The step whose absence leaves the old root unreferenced: the new root
     * must actually point at it. */
    bitter_node_insert(new_root_node->b_data, 0, &old_root_key,
                       bt_get_le64(&leaf_hdr->bytenr),
                       bt_get_le64(&leaf_hdr->generation));

    root->bytenr = bt_get_le64(&((struct bitter_header*)new_root_node->b_data)->bytenr);
    root->level++;
    /* As in btree_split_node: the handle's three fields describe one block and
     * move together.  Silent when an insert reaches here, because the descent
     * already CoW'd the root into this same transaction -- split_root.c calls
     * split directly and is what makes the omission visible. */
    root->generation =
        bt_get_le64(&((struct bitter_header*)new_root_node->b_data)->generation);

    path->nodes[1] = new_root_node;
    path->slots[1] = 0;
    env->ops->dirty_block(env, new_root_node);
  }

  /* Read AFTER the root case, which may have just created it. */
  struct bitter_buf* parent = path->nodes[1];
  bt_u32 parent_slot = (bt_u32)path->slots[1];

  /*
   * Decide before allocating anything.  Once items start moving we are
   * committed, and the fallback wants a different split point entirely --
   * so the choice has to be made while nothing has changed.
   */
  bt_u32 left_free  = (nritems >= 2)
                    ? BITTER_LEAF_DATA_SIZE - bitter_leaf_range_size(buf, 0, mid) : 0;
  bt_u32 right_free = (nritems >= 2)
                    ? BITTER_LEAF_DATA_SIZE - bitter_leaf_range_size(buf, mid, nritems) : 0;
  int use_midpoint  = (nritems >= 2) &&
                      ((slot <  mid && need <= left_free) ||
                       (slot >= mid && need <= right_free));

  /*
   * The parent must have room before anything is allocated.  MIDDLE adds two
   * entries, every other shape one -- so two free slots is the honest
   * requirement.  It is an assert, not an error return: making room is the
   * descent's job (btree_search splits nodes on the way down), so a full parent
   * here is a bug above us, not a condition the disk or the user can produce.
   */
  BITTER_ASSERT(bitter_node_free_slots(parent->b_data) >= 2);

  if (use_midpoint) {

    struct bitter_buf* sib;
    int ar_sib = bitter_alloc_block(env, root, 0, trans, &sib);
    if (ar_sib < 0) {
      return ar_sib;
    }
    void* buf_s = sib->b_data;

    /*
     * Moving items is two regions, not one.
     *
     * The descriptors copy across as a block.  Their payloads occupy a
     * contiguous run -- from item[nritems-1].offset up to item[mid-1].offset --
     * which must land TOP-ANCHORED in the destination, because item[mid]
     * becomes slot 0 and slot 0's payload has to end flush with the end of the
     * data area.  bitter_leaf_check reports ERR_ANCHOR and ERR_GAP if it does
     * not.
     *
     * memcpy, not memmove: two different buffers, so they cannot overlap.
     */
    bt_u32 moved   = nritems - mid;
    bt_u32 payload = bitter_leaf_range_size(buf, mid, nritems) - moved * ITEM;

    memcpy((bt_u8*)buf_s + BITTER_HEADER_SIZE,
           (bt_u8*)buf   + BITTER_HEADER_SIZE + mid * ITEM,
           moved * ITEM);
    memcpy((bt_u8*)buf_s + BITTER_BLOCK_SIZE - payload,
           bitter_leaf_data(buf, nritems - 1),
           payload);

    bt_put_le32(&((struct bitter_header*)buf  )->nritems, mid);
    bt_put_le32(&((struct bitter_header*)buf_s)->nritems, moved);

    /*
     * THE step that is easy to omit: the copied descriptors still carry
     * offsets relative to the SOURCE block.  Because the run keeps its
     * relative arrangement, one uniform delta corrects them all -- the
     * difference between where the run ended in the source and where it must
     * end in the destination.
     */
    bt_u32 delta = BITTER_LEAF_DATA_SIZE - bitter_item_get_offset(bitter_leaf_item(buf, mid - 1));
    for (bt_u32 i = 0; i < moved; i++) {
      struct bitter_item* it = bitter_leaf_item(buf_s, i);
      bitter_item_set_offset(it, bitter_item_get_offset(it) + delta);
    }

    /* The sibling holds the larger keys, so its entry sorts immediately after
     * the original's.  Without this it is unreachable. */
    struct bitter_key_cpu sib_key;
    bitter_key_from_disk(&sib_key, &bitter_leaf_item(buf_s, 0)->key);
    bitter_node_insert(parent->b_data, parent_slot + 1, &sib_key,
                       bt_get_le64(&((struct bitter_header*)buf_s)->bytenr),
                       bt_get_le64(&((struct bitter_header*)buf_s)->generation));

    env->ops->dirty_block(env, leaf);
    env->ops->dirty_block(env, sib);

    if (slot >= mid) {
      /* The item belongs in the sibling, so the path follows it.  slots[1]
       * must move too: fixup_low_keys walks up through it, and pointing at the
       * original would rewrite the wrong separator. */
      path->nodes[0] = sib;
      path->slots[0] = (int)(slot - mid);
      path->slots[1] = (int)(parent_slot + 1);
      env->ops->put_block(env, leaf);     /* leaves the path, so ours */
    } else {
      env->ops->put_block(env, sib);      /* never entered the path */
    }

  } else if (slot == nritems) {

    /* APPEND.  One new leaf holding only the item, after the original.
     * Nothing moves inside the original at all. */
    struct bitter_buf* a;
    int ar_a = bitter_alloc_block(env, root, 0, trans, &a);
    if (ar_a < 0) {
      return ar_a;
    }

    bitter_node_insert(parent->b_data, parent_slot + 1, key,
                       bt_get_le64(&((struct bitter_header*)a->b_data)->bytenr),
                       bt_get_le64(&((struct bitter_header*)a->b_data)->generation));

    env->ops->dirty_block(env, a);
    path->nodes[0] = a;
    path->slots[0] = 0;
    path->slots[1] = (int)(parent_slot + 1);
    /* The original was not modified, so it is released clean. */
    env->ops->put_block(env, leaf);

  } else if (slot == 0) {

    /* PREPEND.  One new leaf, BEFORE the original -- inserted at the
     * original's own slot, which pushes it along. */
    struct bitter_buf* a;
    int ar_a = bitter_alloc_block(env, root, 0, trans, &a);
    if (ar_a < 0) {
      return ar_a;
    }

    bitter_node_insert(parent->b_data, parent_slot, key,
                       bt_get_le64(&((struct bitter_header*)a->b_data)->bytenr),
                       bt_get_le64(&((struct bitter_header*)a->b_data)->generation));

    env->ops->dirty_block(env, a);
    path->nodes[0] = a;
    path->slots[0] = 0;
    path->slots[1] = (int)parent_slot;
    env->ops->put_block(env, leaf);

    /*
     * The only shape where split owes a fixup.  A new leaf went in ahead of
     * the original, so if the original was the parent's leftmost child, the
     * parent's own first key has just changed -- and btree_insert's check
     * afterwards looks at the leaf, not the parent, so it cannot catch this.
     */
    env->ops->dirty_block(env, parent);
    fixup_low_keys(env, path, key, 1);

  } else {

    /*
     * MIDDLE.  The item sorts strictly between two existing items and fits in
     * neither half, so it gets a block of its own between them:
     *
     *     parent:  [ original ]   [ A ]   [ B ]
     *              items 0..slot-1  item   items slot..n-1
     *
     * Both allocations happen before anything moves, so a failure on either
     * leaves the original untouched and the error path is a plain release.
     */
    struct bitter_buf* a;
    int ar_a = bitter_alloc_block(env, root, 0, trans, &a);
    if (ar_a < 0) {
      return ar_a;
    }
    struct bitter_buf* b;
    int ar_b = bitter_alloc_block(env, root, 0, trans, &b);
    if (ar_b < 0) {
      /* `a` is ours and nothing points at it -- release it CLEAN, since
       * writing it back would persist a leaf no parent references.  The block
       * it consumed is not returned to the free map: a failed split leaks it
       * until the next mount rebuilds the map from the extent tree.  See
       * docs/LOG.md. */
      env->ops->put_block(env, a);
      return ar_b;
    }
    void* buf_b = b->b_data;

    /* B takes [slot, nritems), the same two-region move as the midpoint case
     * but split at the insertion slot rather than the balance point. */
    bt_u32 moved   = nritems - slot;
    bt_u32 payload = bitter_leaf_range_size(buf, slot, nritems) - moved * ITEM;

    memcpy((bt_u8*)buf_b + BITTER_HEADER_SIZE,
           (bt_u8*)buf   + BITTER_HEADER_SIZE + slot * ITEM,
           moved * ITEM);
    memcpy((bt_u8*)buf_b + BITTER_BLOCK_SIZE - payload,
           bitter_leaf_data(buf, nritems - 1),
           payload);

    bt_put_le32(&((struct bitter_header*)buf  )->nritems, slot);
    bt_put_le32(&((struct bitter_header*)buf_b)->nritems, moved);

    bt_u32 delta = BITTER_LEAF_DATA_SIZE - bitter_item_get_offset(bitter_leaf_item(buf, slot - 1));
    for (bt_u32 i = 0; i < moved; i++) {
      struct bitter_item* it = bitter_leaf_item(buf_b, i);
      bitter_item_set_offset(it, bitter_item_get_offset(it) + delta);
    }

    /* A at parent_slot + 1, then B at + 2 -- keys ascend across the three. */
    bitter_node_insert(parent->b_data, parent_slot + 1, key,
                       bt_get_le64(&((struct bitter_header*)a->b_data)->bytenr),
                       bt_get_le64(&((struct bitter_header*)a->b_data)->generation));

    struct bitter_key_cpu b_key;
    bitter_key_from_disk(&b_key, &bitter_leaf_item(buf_b, 0)->key);
    bitter_node_insert(parent->b_data, parent_slot + 2, &b_key,
                       bt_get_le64(&((struct bitter_header*)buf_b)->bytenr),
                       bt_get_le64(&((struct bitter_header*)buf_b)->generation));

    env->ops->dirty_block(env, leaf);
    env->ops->dirty_block(env, a);
    env->ops->dirty_block(env, b);

    path->nodes[0] = a;
    path->slots[0] = 0;
    path->slots[1] = (int)(parent_slot + 1);

    /* Both leave the path; both are ours to release. */
    env->ops->put_block(env, leaf);
    env->ops->put_block(env, b);
  }

  env->ops->dirty_block(env, parent);
  return 0;
}

/*
 * Moves `path` from the leaf it is on to the next leaf in key order.
 *
 * Returns 0 on success, 1 when there is no next leaf, or a negative error.
 * The 1 follows btree_search: the end of a tree is an answer, not a failure.
 *
 * On both 1 and an error the path is released, so every non-zero return leaves
 * it in one known state and the caller frees nothing.
 *
 * The walk is up-then-down.  Climb until a level is found whose slot has a
 * successor -- releasing each level as it is left, since it can never be
 * revisited -- then step that slot forward and descend LEFTMOST from the new
 * child to level 0.  The descent is the half that is easy to omit: arriving at
 * level i only repopulates that level, and everything below it was released on
 * the way up.
 */
int btree_next_leaf(struct bitter_env* env, struct bitter_root* root,
        struct bitter_path* path) {

  /* Moves an existing position; it does not create one.  A path that is not on
   * a leaf is the caller's bug. */
  BITTER_ASSERT(path->nodes[0]);

  bt_u8 level = root->level;

  for (bt_u8 i = 0; i < level; i++) {

    /* Left behind for good: the walk only ever moves forward. */
    env->ops->put_block(env, path->nodes[i]);
    path->nodes[i] = 0;

    struct bitter_buf* parent = path->nodes[i + 1];
    bt_u32 slot    = (bt_u32)path->slots[i + 1];
    bt_u32 nritems = bitter_leaf_nritems(parent->b_data);

    /* slot + 1 < nritems, never slot < nritems - 1: nritems is unsigned, and
     * an empty node would wrap the subtraction to 4294967295. */
    if (slot + 1 >= nritems) {
      continue;
    }

    /* The branch point.  Recording the step is what stops the next call from
     * finding this same slot again and returning this same leaf forever. */
    path->slots[i + 1] = (int)(slot + 1);

    /* Leftmost descent from the new child down to level 0. */
    bt_u64 bytenr = bitter_node_blockptr(parent->b_data, slot + 1);

    for (bt_u8 d = i; ; d--) {
      struct bitter_buf* buf = env->ops->read_block(env, bytenr);
      if (!buf) {
        bitter_path_release(env, path);
        return -BITTER_EIO;
      }
      path->nodes[d] = buf;
      path->slots[d] = 0;
      if (d == 0) {
        break;
      }
      bytenr = bitter_node_blockptr(buf->b_data, 0);
    }

    return 0;
  }

  /* Walked off the top: every level below the root has been released and
   * nulled, so releasing the rest leaves the path empty. */
  bitter_path_release(env, path);
  return 1;
}


/*
 * The highest objectid present in `root` -- see core/btree.h for the contract.
 *
 * --- the search key -------------------------------------------------------
 * The point of this key is that NOTHING can sort after it, so the descent
 * takes the rightmost child at every level and lands in the last leaf.  All
 * three fields matter, and the type is the subtle one.
 *
 *   objectid  BITTER_LAST_FREE_OBJECTID, the top of the usable range.  A real
 *             inode may legitimately hold this number.
 *   type      0xFF.  NOT 0 -- keys sort (objectid, type, offset) with type
 *             dominating offset, so (LAST_FREE, 0, 0) sorts BEFORE
 *             (LAST_FREE, INODE_ITEM, 0).  Searching with type 0 would land
 *             before the items of an object numbered LAST_FREE, step back past
 *             them, and report the SECOND-highest object -- after which the
 *             caller hands out a number already in use.
 *   offset    ~0, the same argument one level down.
 *
 * Unreachable until something creates a file, which is what makes it worth
 * writing down rather than discovering.
 *
 * --- why no backward walk -------------------------------------------------
 * btree_search steps to the child BEFORE the first separator greater than the
 * key, so a key greater than everything lands in the last leaf by
 * construction.  Forward scans need btree_next_leaf; this does not.
 *
 * That also gives slots[0] == 0 a precise meaning here: since the key exceeds
 * every real one, the in-leaf position can only be 0 when the leaf holds no
 * items -- and it is the LAST leaf, so the tree is empty.
 */
int bitter_find_last_objectid(struct bitter_env* env, struct bitter_root *root,
                bt_u64 *out) {

  struct bitter_path path;
  struct bitter_key_cpu key;
  struct bitter_key_cpu last_key;
  void* buf;
  struct bitter_item *it;

  bt_u32 slot;
  int s;

  key.objectid = BITTER_LAST_FREE_OBJECTID;
  key.type = 0xFF;
  key.offset = ~0ULL;

  bitter_path_init(&path);

  /* ins_len 0, trans 0, cow 0 -- the null trans is the proof this cannot
   * reach any code that writes. */
  s = btree_search(env, root, &key, &path, 0, 0, 0);
  if (s < 0) {
    /* btree_search released the path on its own error returns. */
    return s;
  }

  /*
   * An exact match on a key whose type is 0xFF.  The defined types stop at
   * BITTER_EXTENT_ITEM (144), so no writer could have produced this -- and it
   * matters beyond tidiness: on s == 0 the step back below does not happen, so
   * the answer would be LAST_FREE_OBJECTID itself and the caller's +1 would
   * leave the usable range entirely.
   */
  if (s == 0) {
    bitter_path_release(env, &path);
    return -BITTER_EUCLEAN;
  }

  /* Empty tree.  -BITTER_ENOENT rather than 0 in *out, because 0 is
   * BITTER_INVALID_OBJECTID and a caller ignoring the return would allocate
   * from the reserved range. */
  if (path.slots[0] == 0) {
    bitter_path_release(env, &path);
    return -BITTER_ENOENT;
  }

  buf = path.nodes[0]->b_data;

  /* slots[0] is the insertion point, one PAST the last item. */
  slot = (bt_u32)(path.slots[0] - 1);

  it = bitter_leaf_item(buf, slot);

  /* Decoded rather than read in place: the key on disk is little-endian and
   * `it` points into a buffer the release below drops. */
  bitter_key_from_disk(&last_key, &it->key);

  *out = last_key.objectid;
  bitter_path_release(env, &path);
  return 0;
}
