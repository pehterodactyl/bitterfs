#include "core/cow.h"
#include "core/trans.h"
#include "core/alloc.h"
#include "core/bitter_string.h"
#include "core/bitter_assert.h"

int btree_cow_block(struct bitter_env* env, struct bitter_root* root, struct bitter_path* path,
      bt_u8 level, struct bitter_trans* trans) {

  /* The descent's own bound, not the disk's: btree_search rejects an
   * out-of-range root->level before the loop, and level only falls from there. */
  BITTER_ASSERT(level < BITTER_MAX_LEVEL);

  struct bitter_buf* buf_a = path->nodes[level];
  struct bitter_header* hdr = buf_a->b_data;
  bt_u64 old_bytenr = buf_a->b_bytenr;
  
  /* Already this transaction's, so nobody else can be pointing at it: writable
   * in place, and whoever put it here already patched the parent. */
  if (bt_get_le64(&hdr->generation) == trans->generation) {
    return 0;
  }

  struct bitter_buf* buf_temp;
    int ar_buf_temp = bitter_alloc_block(env, root, level, trans, &buf_temp);
  if (ar_buf_temp < 0) {
      return ar_buf_temp;
    }

  /* Whole block, header included -- nritems, owner and level are wanted as they
   * are, and the two fields that are not survive outside it: the address in
   * b_bytenr, the generation in trans. */
  memcpy(buf_temp->b_data, buf_a->b_data, BITTER_BLOCK_SIZE);

  struct bitter_header* new_hdr = buf_temp->b_data;
  bt_put_le64(&new_hdr->bytenr, buf_temp->b_bytenr);
  bt_put_le64(&new_hdr->generation, trans->generation);

  /* Into the path BEFORE the release: the path is the only thing tracking
   * buf_a, so overwriting the slot is what makes it ours to put. */
  path->nodes[level] = buf_temp;
  
  env->ops->put_block(env, buf_a);
  env->ops->dirty_block(env, buf_temp);

  /* Nothing points at the copy yet.  Either the parent's key_ptr or, at the
   * root, root->bytenr -- and one of the two must happen or the entire subtree
   * below this block is unreachable. */
  if (level != root->level) {
    struct bitter_buf* p_buf = path->nodes[level + 1];
    struct bitter_key_ptr* p_key = bitter_node_key_ptr(p_buf->b_data,
              path->slots[level + 1]);

    /* Both halves: the key_ptr duplicates the child's generation so a stale
     * pointer is detectable without reading the child. */
    bt_put_le64(&p_key->blockptr, buf_temp->b_bytenr);
    bt_put_le64(&p_key->generation, trans->generation);
    env->ops->dirty_block(env, p_buf);
  }
  else {
    /* In memory only.  The superblock is written once, at commit -- writing it
     * here would publish a tree whose blocks are not durable yet. */
    root->bytenr = buf_temp->b_bytenr;
    root->generation = trans->generation;

  }

  /*
   * The original is now unreferenced -- the parent, or root->bytenr, points at
   * the copy instead -- so release it.  Deferred, like the allocation:
   * decrementing the count here would descend the extent tree, which
   * copy-on-writes it, which lands back in this function.
   *
   * LAST, after every structural step.  A failure at this point means the
   * allocation was recorded and the free was not, which leaks the block until
   * the next mount rebuilds the map from the extent tree.  The other order
   * would risk the opposite -- a free recorded without its allocation -- and
   * that hands one block to two owners.  The caller aborts either way; the two
   * are not equally bad.
   *
   * Unconditional, and only correct because every tree block has exactly one
   * parent.  The skeleton's third case -- a block SHARED with a snapshot, which
   * must be copied and NOT freed -- needs the refcount read before deciding,
   * and reading it from here is the same recursion again.  Phase 6, and
   * docs/LOG.md carries the entry.
   */
  int s = trans_add_delayed_ref(trans, old_bytenr, BITTER_BLOCK_SIZE, -1);
  if (s < 0) {
    return s;
  }
  return 0;
}
