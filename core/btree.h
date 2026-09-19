/*
 * The B-tree: search, insert, delete, split, merge.
 *
 * The dividing line against items.c is buffers.  items.c knows the layout of
 * ONE block and takes a raw void *, so it can be tested against a bare 4096
 * byte array.  Everything here reads blocks through the environment, holds
 * them in a path, and must release them again -- so everything here takes a
 * struct bitter_env *.
 *
 * Phase 2 modifies blocks IN PLACE.  No copy-on-write, no generations, no
 * transactions: those arrive in phase 3, and adding them to a B-tree that
 * already works is a contained change where debugging both at once is not.
 */
#ifndef BITTER_BTREE_H
#define BITTER_BTREE_H

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/items.h"

/* Pointer-only in this header: the open transaction every mutating entry point
 * carries, so that a function without one cannot change the tree. */
struct bitter_trans;

/* Pointer-only for a harder reason: core/fs.h holds two bitter_roots BY VALUE,
 * so it must include this header.  Including it back would be a cycle -- the
 * second guard wins and whichever struct is parsed first sees the other as
 * incomplete.  A forward declaration cannot participate in one. */
struct bitter_fs_info;

/*
 * The path from root to leaf that a search walks, and the central structure of
 * btree.c.  One buffer and one index per level.
 *
 * It is search's OUTPUT: btree_search does not return an item, it fills a path
 * and reports whether the key was found.  nodes[0] is then the leaf and
 * slots[0] the position — of the item if it exists, or of where it would be
 * inserted if it does not.
 *
 * It is also insert's INPUT, because a split propagates upward and every
 * ancestor needs a new key_ptr.  Without the path, insert would have to search
 * again at each level.
 *
 * And it is what makes buffer lifetime tractable.  Every nodes[i] came from
 * read_block and must reach put_block exactly once, INCLUDING when a search
 * fails halfway down; one release function looping over this array is what
 * turns that into a single call rather than eight goto labels.
 *
 * btrfs's equivalent carries a locks[] array and seven flag bits for lock
 * behaviour.  None of that appears here because core/ never locks — the kernel
 * module holds one mutex around every entry point (see docs, "Locking").  That
 * simplification is most of why this struct has two fields where theirs has
 * fifteen.
 */
struct bitter_path {
  
  struct bitter_buf *nodes[BITTER_MAX_LEVEL];

  int                slots[BITTER_MAX_LEVEL];

};


/*
 * One tree: where its root block lives and how deep it is.
 *
 * Passed by pointer and updated in place because a root MOVES.  When the root
 * block splits, the tree gains a level and a brand new block becomes the root,
 * so insert has to hand both facts back.  From phase 4 there are three of these
 * -- root tree, extent tree, FS tree -- and every operation names one.
 *
 * level 0 means the root IS a leaf, which is what mkfs produces.
 */
struct bitter_root {

  bt_u64 bytenr;

  bt_u8 level;

  /*
   * Which tree this is: BITTER_ROOT_TREE_OBJECTID, and from phase 4 also the
   * extent tree and the FS tree.
   *
   * Stamped into header.owner of every block allocated for this tree, so that
   * blocks a split creates are indistinguishable from the ones mkfs wrote.
   * Nothing reads it yet -- neither checker tests it and search does not --
   * but fsck compares it against a full tree walk at phase 4, and a block
   * cross-linked into two trees is a failure that checksum, bytenr and fsid
   * all miss: the block is intact, in the right place, and belongs to somebody
   * else.
   */
  bt_u64 objectid;

  /*
   * --- phase 2 only: the allocator ---------------------------------------
   *
   * Split needs a fresh block, and alloc_block_buf takes the address as an
   * argument, so something has to choose it.  There is no extent tree until
   * phase 4, so these two fields stand in for one: a high-water mark and the
   * device size that bounds it.
   *
   * Every allocation returns next_free and advances it by BITTER_BLOCK_SIZE.
   * Nothing is ever reclaimed -- a leaf that empties stays allocated forever.
   * Fine in phase 2, where nothing deletes enough to notice.
   *
   * total_bytes is what makes "out of space" answerable at ALLOCATION time
   * rather than as a failed pwrite three calls later.  It mirrors
   * super.total_bytes; at phase 5 mount would read it from there.
   *
   * Deliberately crude and deliberately in an obviously temporary place:
   * phase 4 deletes two fields rather than unpicking an interface.
   */
  bt_u64 next_free;
  bt_u64 total_bytes;

  //Gen 3 introduction, generation remembers at what stage this project is supposed to be at
  bt_u64 generation;

  struct bitter_fs_info* fs_info;

};

/*
 * Zeroes a path.  Must be called once before a path's first use: release()
 * calls put_block on every non-NULL entry, so handing it an uninitialised
 * stack path would release garbage pointers.
 *
 * After this, the same path can be reused across any number of searches --
 * btree_search releases it on entry.
 */
void bitter_path_init(struct bitter_path *path);

/*
 * Drops every buffer the path holds and clears it.
 *
 * Must be reachable from every exit of every function that fills a path,
 * including the ones that fail partway down -- an unreadable child, a block
 * that fails its checker.  Those are precisely the paths least likely to be
 * exercised, which is why this exists as one call rather than as a put_block
 * before each return.
 *
 * Safe to call twice, and safe on a path that was never filled, so a caller
 * can release unconditionally rather than tracking whether it needs to.
 */
void bitter_path_release(struct bitter_env *env, struct bitter_path *path);

/*
 * See core/btree.c for the full contract.  `ins_len` is the size of an item
 * the caller means to insert, or 0 for a pure lookup; non-zero asks the
 * descent to make room as it goes.
 *
 * `trans` may be null for a pure lookup and must not be for a splitting one --
 * a split allocates, and an allocation needs a generation to stamp.
 */
int btree_search(struct bitter_env *env, struct bitter_root *root,
          const struct bitter_key_cpu *key, struct bitter_path *path,
          bt_u32 ins_len, struct bitter_trans *trans, bt_u8 cow);

/*
 * The highest objectid present in `root`, through `out`.
 *
 * Keys sort (objectid, type, offset) with objectid first, so the LAST item in
 * the tree belongs to the highest-numbered object -- one search for a key
 * beyond every real one, landing past the end of the last leaf.
 *
 * A pure lookup: no trans, no cow, nothing written.
 *
 * Returns 0, or a negative error.  An EMPTY tree is -BITTER_ENOENT rather than
 * a zero in `out`, because 0 is BITTER_INVALID_OBJECTID and a caller that
 * ignored the return would then allocate from the reserved range.
 */
int bitter_find_last_objectid(struct bitter_env *env, struct bitter_root *root,
          bt_u64 *out);

/*
 * Inserts an item into the tree, splitting whatever is full on the way.
 *
 * Returns 0, or a negative error:
 *
 *   -BITTER_EEXIST   the key is already present.  The keyspace is unique within
 *                    a tree, so this is a caller error -- but a reportable one,
 *                    since phase 5's create() legitimately hits it when two
 *                    processes race for the same name.
 *   -BITTER_EINVAL   `size` exceeds BITTER_MAX_ITEM_SIZE.  Checked before the
 *                    search, so a rejected insert mutates nothing.
 *   -BITTER_ENOSPC   the device is full -- a split needed a block and the
 *                    allocator had none.
 *   -BITTER_ENOMEM   the environment could not give a split a buffer.
 *   -BITTER_EIO
 *   -BITTER_EUCLEAN  passed through from the search.
 *
 * `path` is scratch space rather than an output: it must be initialised, and it
 * is released before returning on every path including the error ones.  It is a
 * parameter only because it is large -- eight buffers and eight slots -- and a
 * caller may want to reuse one across many inserts.
 *
 * --- what it owes after a successful insert --------------------------------
 *
 * 1. dirty_block on every buffer it modified.  Nothing visibly breaks without
 *    this: the change is made in memory and then silently discarded, which the
 *    env_user tests already demonstrate.
 *
 * 2. fixup_low_keys when the item landed at slot 0, because a leaf's first key
 *    is duplicated in its parent as a separator -- and in the grandparent too,
 *    if the leaf was its parent's leftmost child.  This is the failure neither
 *    bitter_leaf_check nor bitter_node_check can catch: every block stays
 *    individually valid and only the relationship between two of them is wrong,
 *    after which searches route past items that exist.
 *
 * 3. Release the path.
 *
 * --- the room test ---------------------------------------------------------
 * A leaf has room when its free space covers the payload AND the new descriptor
 * -- the same sum bitter_leaf_insert asserts on.  When it does not, the leaf is
 * split and the item goes into whichever half the path was left pointing at.
 *
 * The split needs no room test of its own in the parent: btree_insert passes a
 * non-zero ins_len to btree_search, and that descent leaves every node it
 * passed through holding at least BITTER_INSERT_SLACK free entries.  This is
 * why splitting happens top-down -- by the time a leaf is found to be full, it
 * is far too late to go back and make room above it.
 *
 * MISSING FROM THE SIGNATURE: `const void *data` and `bt_u32 size`.  Without
 * them there is no payload to insert and no way to ask whether it fits.  The
 * same gap bitter_leaf_insert had.
 */
int btree_insert(struct bitter_env *env, struct bitter_root* root, 
        const struct bitter_key_cpu* key, struct bitter_path* path, const void* data, bt_u32 size, 
        struct bitter_trans* trans);

int btree_split_node(struct bitter_env* env,struct bitter_path* path, bt_u8 level, 
        struct bitter_root* root, struct bitter_trans* trans);

int btree_split_leaf(struct bitter_env* env, struct bitter_path* path, struct bitter_root* root, 
        const struct bitter_key_cpu* key, bt_u32 size, struct bitter_trans* trans);

/*
 * Removes the item with `key` from the tree.
 *
 * Returns 0, or a negative error:
 *
 *   -BITTER_ENOENT   the key is not in the tree.  Constructed here rather than
 *                    reported by the search: btree_search returns 1 for a slot
 *                    that would hold the key, which is the normal result for
 *                    insert and a failure only for delete.
 *   -BITTER_EIO
 *   -BITTER_EUCLEAN  passed through from the search.
 *
 * `root` is needed only to reach btree_search; delete never allocates.  For the
 * same reason the search is given ins_len 0 -- splitting a node on the way down
 * to a removal would grow the tree on the one path meant to shrink it.
 *
 * `path` is scratch space, released before returning on every path including
 * the error ones -- the not-found case included, where the search succeeded and
 * left a buffer held at every level.
 *
 * --- what it owes after a successful delete --------------------------------
 *
 * 1. dirty_block on the leaf.  fixup_low_keys dirties the nodes it touches, but
 *    nothing dirties the leaf for you, and without it the removal is made in
 *    memory and then silently discarded.
 *
 * 2. fixup_low_keys when the item was at slot 0 AND the leaf still holds items,
 *    passing the leaf's NEW first key -- the item that shifted down into slot
 *    0, read after the removal, not the key just deleted.  nritems must
 *    therefore be captured BEFORE bitter_leaf_remove, which is the function's
 *    only real ordering constraint.
 *
 * 3. Release the path.
 *
 * --- the empty leaf --------------------------------------------------------
 * A leaf emptied by the last delete stays in the tree, and its parent keeps the
 * separator naming the key just removed.  That is safe: a separator is a lower
 * bound on a range, not a claim that an item exists, and the bound still sits
 * strictly between the neighbouring leaves' keys, so the node stays ascending.
 * A search landing there finds nothing, which is the right answer; a later
 * insert into that range lands there and fixup_low_keys repairs the separator
 * on the way out.  This is why the slot-0 case skips the fixup rather than
 * looking for a substitute key -- there is no slot 0 left to read.
 *
 * The cost is real but is shape, not correctness: every emptied leaf holds a
 * block and a parent slot forever, so enough deletes fill nodes with pointers
 * to nothing and force splits that deepen a tree holding fewer items.  Merging
 * is what would reclaim them, and it is deferred -- see docs/LOG.md.
 */
int btree_del_item(struct bitter_env *env, struct bitter_root *root, 
    const struct bitter_key_cpu *key, struct bitter_path *path,
    struct bitter_trans *trans);

int btree_next_leaf(struct bitter_env* env, struct bitter_root* root, 
        struct bitter_path* path);

#endif
