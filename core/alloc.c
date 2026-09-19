#include "core/alloc.h"
#include "core/trans.h"
#include "core/fs.h"
#include "core/bitter_endian.h"
#include "core/bitter_string.h"
#include "core/bitter_assert.h"


int bitter_alloc_block(struct bitter_env* env, struct bitter_root* root,
        bt_u8 level, struct bitter_trans* trans, struct bitter_buf** out) {

  /* Cleared on every failure path, so a caller who tests the pointer rather
   * than the return still fails safe. */
  *out = 0;

  /* Set at mount, by whoever built the fs_info.  A root without one is a
   * wiring bug in core/, not anything the disk did. */
  struct bitter_fs_info* fs = root->fs_info;
  BITTER_ASSERT(fs);

  /* An empty map IS out of space.  This replaces the
   * next_free + BLOCK_SIZE > total_bytes expression the callers used to
   * repeat, and needs no arithmetic. */
  if (fs->free_count == 0) {
    return -BITTER_ENOSPC;
  }

  /* BY POINTER: the entry is shrunk in place.  Taking a copy would leave the
   * map untouched and every allocation would return the same address. */
  struct bitter_free_extent* ext = &fs->free[0];
  bt_u64 addr = ext->start;

  /* The map is built by core/'s own mount scan, so a malformed entry here is
   * our bug -- assert.  The scan validates the disk and returns -BITTER_EUCLEAN
   * instead, because there the same values came from outside. */
  BITTER_ASSERT(addr % BITTER_BLOCK_SIZE == 0);
  BITTER_ASSERT(ext->length > 0);
  BITTER_ASSERT(ext->length % BITTER_BLOCK_SIZE == 0);

  struct bitter_buf* buf = env->ops->alloc_block_buf(env, addr);
  if (!buf) {
    return -BITTER_ENOMEM;
  }

  /* Consumed only now that the buffer exists: a failed alloc_block_buf must
   * leave the block available rather than losing it. */
  ext->start  += BITTER_BLOCK_SIZE;
  ext->length -= BITTER_BLOCK_SIZE;

  if (ext->length == 0) {
    /* Range exhausted -- drop entry 0 and shift the rest down.  memmove, not
     * memcpy: source and destination overlap. */
    fs->free_count--;
    memmove(&fs->free[0], &fs->free[1],
            (bt_size)fs->free_count * sizeof(struct bitter_free_extent));
  }

  /*
   * Record the allocation so it reaches the extent tree at commit.  Deferred
   * rather than done here: calling extent_inc_ref would descend the extent
   * tree, which copy-on-writes it, which lands back in this function.  The
   * block is already out of the free map, so nothing else can be handed the
   * same address before the set is drained.
   *
   * Skipping this is not a leak, it is corruption: the map is rebuilt from the
   * extent tree at mount, so an unrecorded block is free again next time and
   * gets handed to a second owner.  See docs/LOG.md.
   */
  int r = trans_add_delayed_ref(trans, addr, BITTER_BLOCK_SIZE, +1);
  if (r < 0) {
    /* The block stays consumed -- it cannot go back, because the entry that
     * would have described it is exactly what failed.  A caller seeing this
     * must abort the transaction, and the next mount's rescan recovers the
     * block.  Released so the buffer is not leaked; nothing references it. */
    env->ops->put_block(env, buf);
    return r;
  }

  *out = buf;

  struct bitter_header* hdr = buf->b_data;
  bt_put_le64(&hdr->bytenr, addr);
  bt_put_le32(&hdr->nritems, 0);
  bt_put_le64(&hdr->owner, root->objectid);
  hdr->level = level;
  bt_put_le64(&hdr->generation, trans->generation);

  return 0;
}

/*
 * One block off the free map for file data.  See core/alloc.h for why this is
 * not bitter_alloc_block with an argument.
 *
 * The free-map half below is line-for-line what bitter_alloc_block does; what
 * is absent is the point.  No alloc_block_buf, so no env and no buffer.  No
 * header stamped into the block -- those bytes belong to the file.  And no
 * ordering worry about when to consume the map entry: bitter_alloc_block
 * deliberately takes it only AFTER its buffer allocation succeeds, so a
 * failure does not lose the block, and with no buffer nothing can fail in
 * between.
 */
int bitter_alloc_data_block(struct bitter_root* root, struct bitter_trans* trans, bt_u64 *out) {

  /* Cleared first, so every return below leaves it safe without saying so. */
  *out = 0;

  /* Set at mount, by whoever built the fs_info.  A root without one is a
   * wiring bug in core/, not anything the disk did. */
  struct bitter_fs_info *fs = root->fs_info;
  BITTER_ASSERT(fs);
  
  if (fs->free_count == 0) {
    return -BITTER_ENOSPC;
  }

  /* BY POINTER: the entry is shrunk in place.  Taking a copy would leave the
   * map untouched and every allocation would return the same address -- a bug
   * with no symptom until the second call. */
  struct bitter_free_extent* ext = &fs->free[0];
  bt_u64 addr = ext->start;

  /* The map is built by core/'s own mount scan, so a malformed entry here is
   * our bug -- assert.  The scan validates the disk and returns -BITTER_EUCLEAN
   * instead, because there the same values came from outside. */
  BITTER_ASSERT(addr % BITTER_BLOCK_SIZE == 0);
  BITTER_ASSERT(ext->length > 0);
  BITTER_ASSERT(ext->length % BITTER_BLOCK_SIZE == 0);

  ext->start  += BITTER_BLOCK_SIZE;
  ext->length -= BITTER_BLOCK_SIZE;

  if (ext->length == 0) {
    /* Range exhausted -- drop entry 0 and shift the rest down.  memmove, not
     * memcpy: source and destination overlap. */
    fs->free_count--;
    memmove(&fs->free[0], &fs->free[1],
            (bt_size)fs->free_count * sizeof(struct bitter_free_extent));
  }

  /*
   * Record the allocation so it reaches the extent tree at commit.  Deferred
   * rather than done here for the reason bitter_alloc_block gives: touching the
   * extent tree would copy-on-write it, which allocates, which lands back here.
   *
   * Skipping it is not a leak, it is corruption -- the free map is rebuilt from
   * the extent tree at mount, so an unrecorded block is free again next time
   * and gets handed to a second owner.  See docs/LOG.md.
   *
   * No cleanup on failure, unlike bitter_alloc_block, which has a buffer to
   * put_block here.  Nothing was allocated.  The block stays consumed -- it
   * cannot go back, because the entry that would have described it is exactly
   * what failed -- and the next mount's rescan recovers it.
   */
  int r = trans_add_delayed_ref(trans, addr, BITTER_BLOCK_SIZE, +1);
  if (r < 0) {
    return r;
  }

  /* Last, so a caller that tests the pointer rather than the return never sees
   * an address the transaction does not know about. */
  *out = addr;
  return 0;
}
