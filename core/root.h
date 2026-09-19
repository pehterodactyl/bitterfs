#ifndef BITTER_ROOT_H
#define BITTER_ROOT_H

#include "format/bitterfs_format.h"
#include "core/btree.h"
#include "core/bitter_env.h"
#include "core/items.h"

/*
 * Finds the tree named `objectid` and fills `out` with a usable handle to it.
 *
 * The two roots do different jobs: `tree_root` is the tree being SEARCHED --
 * always the root tree -- and `out` is the tree being DESCRIBED.  The root tree
 * itself is not findable this way; it has no root item, because the item would
 * have to live inside the tree it locates.  It comes from the superblock, via
 * bitter_read_super.
 *
 * Returns 0, or a negative error:
 *
 *   -BITTER_ENOENT   no such tree.  btree_search reports not-found as 1
 *                    because insert wants the slot; a caller asking where the
 *                    extent tree lives cannot use that, so it is translated.
 *   -BITTER_EUCLEAN  an item of the wrong length under a BITTER_ROOT_ITEM key.
 *                    The disk's fault, not the caller's -- hence a return
 *                    rather than an assert.
 *
 * A pure lookup: it opens no transaction and writes nothing.  `out` is left
 * untouched unless the call succeeds.
 */
/*
 * Builds the key a tree's root item is stored under: (objectid, ROOT_ITEM, 0).
 *
 * Exists so the key has ONE construction site.  Lookup, mkfs and the commit-
 * time update must all name the same key exactly, and three hand-written
 * copies stay in agreement only until one of them doesn't -- a mismatch here
 * is a lookup that misses a root item that is present.
 *
 * offset is 0 and means "unused" for this type.  Phase 6 may give it a
 * meaning (btrfs stores the creating transaction there, so a tree's roots sort
 * in creation order); whatever it becomes, 0 has to keep meaning the live
 * root, because every image already written has a 0 in it.
 */
void bitter_root_key(struct bitter_key_cpu *key, bt_u64 objectid);

int bitter_find_root(struct bitter_env* env, struct bitter_root* tree_root, bt_u64 objectid,
     struct bitter_root* out);



void root_item_to_disk(struct bitter_root_item *dst, const struct bitter_root *src);

int bitter_update_root(struct bitter_env* env, struct bitter_root* tree_root, 
      const struct bitter_root* src, struct bitter_trans* trans);

#endif
