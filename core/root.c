#include "core/root.h"
#include "core/items.h"
#include "core/bitter_endian.h"
#include "core/bitter_string.h"

int bitter_find_root(struct bitter_env* env, struct bitter_root* tree_root, bt_u64 objectid,
      struct bitter_root* out) {

  /* The key names the tree we want; tree_root says where to look for it. */
  struct bitter_key_cpu key;
  bitter_root_key(&key, objectid);

  /* A local path: the caller has no use for the buffers once the three fields
   * are decoded.  init before first use -- release() walks the array
   * unconditionally, so stack garbage here means put_block on garbage. */
  struct bitter_path path;
  bitter_path_init(&path);

  /* ins_len 0, trans 0, cow 0.  The null trans is the proof this function
   * cannot reach any code that writes. */
  int s = btree_search(env, tree_root, &key, &path, 0, 0 , 0);

  if (s < 0) {
    bitter_path_release(env, &path);
    return s;
  }
  /* Not-found is an answer to search and a failure to us. */
  if (s != 0) {
    bitter_path_release(env, &path);
    return -BITTER_ENOENT;
  }

  struct bitter_buf* buf = path.nodes[0];

  bt_u32 slot = (bt_u32)path.slots[0];
  struct bitter_item* it = bitter_leaf_item(buf->b_data, slot);
  if (bt_get_le32(&it->size) != BITTER_ROOT_ITEM_SIZE) {
    bitter_path_release(env, &path);
    return -BITTER_EUCLEAN;
  }
  struct bitter_root_item* item = bitter_leaf_data(buf->b_data, slot);

  /* `item` points INTO the leaf buffer, so every read below has to happen
   * before the release at the bottom. */
  bt_u64 bytenr = bt_get_le64(&item->bytenr);
  bt_u64 generation = bt_get_le64(&item->generation);

  /* All six fields together, from three sources: the item, the argument, and
   * the filesystem-wide values tree_root already holds.  Written in one run so
   * a failure above can never leave a half-populated root behind. */
  out->bytenr = bytenr;
  out->generation = generation;
  out->level = item->level;         /* one byte: no byte order to convert */
  out->objectid = objectid;
  out->total_bytes = tree_root->total_bytes;

  /* A COPY, and therefore a second independent bump cursor.  Safe only while
   * one tree allocates per transaction; bitter_update_root breaks that.  See
   * docs/LOG.md, "next_free is copied per tree". */
  out->next_free = tree_root->next_free;

  bitter_path_release(env, &path);
  return 0;
}

void root_item_to_disk(struct bitter_root_item *dst, const struct bitter_root *src) {

  bt_put_le64(&dst->bytenr, src->bytenr);
  
  dst->level = src->level;
  
  bt_put_le64(&dst->generation, src->generation);
}

void bitter_root_key(struct bitter_key_cpu *key, bt_u64 objectid) {
  key->objectid = objectid;
  key->type     = BITTER_ROOT_ITEM;
  key->offset   = 0;
}


int bitter_update_root(struct bitter_env* env, struct bitter_root* tree_root,
      const struct bitter_root* src, struct bitter_trans* trans) {

  /* src supplies both halves and is never descended into: its objectid names
   * the item, its other three fields become the payload. */
  struct bitter_key_cpu key;
  memset(&key, 0, sizeof(struct bitter_key_cpu));
  bt_u64 objectid = src->objectid;
  bitter_root_key(&key, objectid);

  struct bitter_path path;
  memset(&path, 0, sizeof(struct bitter_path));
  bitter_path_init(&path);

  /* ins_len 0 because the replacement is the same 17 bytes -- the key never
   * changes, so the item keeps its slot, offset and size and no room has to be
   * made.  cow 1 because this writes: the descent copies every block on the
   * way down and relinks each parent, so tree_root->bytenr may differ by the
   * time this returns.  That moved root is what the superblock records. */
  int s = btree_search(env, tree_root, &key, &path,0,trans, 1);
  
  if (s < 0) {
    bitter_path_release(env, &path);
    return s;
  }
  /* Not-found is an answer to search and a failure to us. */
  if (s != 0) {
    bitter_path_release(env, &path);
    return -BITTER_ENOENT;
  }
  
  struct bitter_buf* bit_buf = path.nodes[0];
  
  bt_u32 slot = (bt_u32)path.slots[0];
  
  /* Validated BEFORE anything is written, and this is a stricter need than the
   * identical check in bitter_find_root: there a wrong size misreads, here it
   * would memcpy 17 bytes over a shorter item and into its neighbour's
   * payload.  bitter_leaf_check would still pass afterwards -- every
   * descriptor stays consistent -- so nothing downstream could detect it.
   * The disk's fault, hence a return and not an assert. */
  struct bitter_item* it = bitter_leaf_item(bit_buf->b_data, slot);
  if (bt_get_le32(&it->size) != BITTER_ROOT_ITEM_SIZE) {
    bitter_path_release(env, &path);
    return -BITTER_EUCLEAN;
  }
  
  struct bitter_root_item item;
  memset(&item, 0, sizeof(struct bitter_root_item));

  root_item_to_disk(&item, src);
  
  /* Overwritten in place rather than removed and reinserted: same key, same
   * size, same slot, so the leaf's layout does not change at all -- nritems,
   * every descriptor's offset and size, and the key all stay as they are.  A
   * remove plus an insert would memmove the whole tail twice to end up
   * byte-identical.
   *
   * No CoW here.  btree_search already copied every block in the path, so this
   * leaf belongs to the current transaction -- the same invariant
   * bitter_leaf_insert and btree_split_node rely on. */
  struct bitter_root_item* payload = bitter_leaf_data(bit_buf->b_data, slot);
  memcpy(payload, &item, sizeof(struct bitter_root_item));
  env->ops->dirty_block(env, bit_buf);  
  bitter_path_release(env, &path);
  return 0;
}

