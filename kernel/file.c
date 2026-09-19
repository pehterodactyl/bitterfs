// SPDX-License-Identifier: GPL-2.0
/*
 * bitterfs — regular files.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/mutex.h>
#include <linux/lockdep.h>
#include <linux/pagemap.h>

#include "format/bitterfs_format.h"
#include "core/bitter_endian.h"
#include "core/bitter_assert.h"
#include "core/btree.h"
#include "core/items.h"
#include "core/alloc.h"
#include "core/file.h"
#include "kernel/bitterfs.h"

static int bitterfs_get_block_create(struct inode *inode, bt_u64 file_offset, 
                                              struct buffer_head *bh_result);

/*
 * Where does file block `iblock` of this inode live on the device?
 *
 * The answer has exactly two forms, and neither is the return value:
 *
 *   map_bh(bh_result, ...)   the data is at that device block
 *   bh_result UNTOUCHED      a hole -- nothing backs this range
 *
 * An unmapped buffer head IS the hole, and mpage_read_folio zero-fills for it.
 * So the sparse case needs no code at all: it is the absence of an answer.  The
 * return value says only whether the question could be answered.
 *
 * --- except under create --------------------------------------------------
 * `create` makes that contract conditional, and it is the one asymmetry in this
 * function:
 *
 *   create == 0   returning 0 unmapped means HOLE
 *   create == 1   returning 0 MUST mean mapped
 *
 * A write has nowhere to land otherwise, and __block_write_begin does not check
 * -- it trusts the return.  So every hole conclusion below converges on one
 * label, which allocates instead of answering "nothing".
 *
 * Note that create does NOT mean "there is no mapping": __block_write_begin
 * calls with create == 1 for every unmapped buffer in the folio, including
 * offsets an EXTENT_DATA item already covers.  Overwriting the middle of a file
 * arrives here exactly like reading it, and takes the same search.
 *
 * Does no I/O on bh_result and never touches its contents -- it describes a
 * location, and the block layer does the reading afterwards.
 *
 * --- the one place a disk value is unguarded ------------------------------
 * Every other read in this module goes through env_kernel.c's read_block,
 * which verifies a checksum, the block's self-recorded bytenr, and the fsid.
 * A DATA block has none of those: no header, nothing self-identifying.
 * Whatever this function names, the block layer reads and hands to userspace
 * as file contents.
 *
 * So the mapping is the only thing that CAN be checked, which is why
 * disk_bytenr is bounded and alignment-checked below.  A corrupt one pointing
 * into the extent tree would otherwise serve metadata as file data and nothing
 * downstream would notice.
 */
static int bitterfs_get_block(struct inode *inode, sector_t iblock,
			      struct buffer_head *bh_result, int create)
{
	struct bitterfs_mount	*mnt = bitterfs_sb(inode->i_sb);
	bt_u64			 ino = inode->i_ino;

	struct bitter_path	 path;
	struct bitter_key_cpu	 key;
	struct bitter_key_cpu	 leaf_key;
	struct bitter_item	*it;
	struct bitter_extent_data *dat;
	void			*block;

	bt_u64			 file_offset;
	bt_u64			 num_bytes;
	bt_u64			 disk_bytenr;
	bt_u64			 ext_offset;
	bt_u64			 device_byte;
	bt_u32			 size;
	int			 slot;
	int			 s;
	int			 err;

	/* The iblock-to-byte step below assumes one folio is one block.  True on
	 * x86-64 by coincidence rather than construction, so it is asserted
	 * rather than relied on silently. */
	BUILD_BUG_ON(BITTER_BLOCK_SIZE != PAGE_SIZE);

	file_offset = (bt_u64) iblock * BITTER_BLOCK_SIZE;

	/*
	 * Past the end of the file is nothing, and cheaper to answer than to
	 * search for.  i_size_read rather than inode->i_size: on 32-bit a
	 * 64-bit loff_t cannot be read atomically.
	 *
	 * !create, and the gate is not an optimisation.  i_size does not grow
	 * until write_end, so during write_begin for an append this test is true
	 * on EVERY call -- ungated it would refuse to allocate for exactly the
	 * case that needs it, and every append would silently map nothing.
	 */
	if (!create && file_offset >= (bt_u64) i_size_read(inode))
		return 0;

	bitter_file_extent_key(&key, ino, file_offset);
	bitter_path_init(&path);

	mutex_lock(&mnt->lock);

	s = btree_search(&mnt->env, &mnt->fs.fs_root, &key, &path, 0, NULL, 0);
	if (s < 0) {
		err = s;
		goto out_unlock;
	}

	/*
	 * This is a CONTAINMENT lookup, not an identity one: the item covering
	 * file_offset is the one with the largest key offset at or below it.
	 *
	 * s == 0 means an item starts exactly here.  s == 1 means none does, and
	 * slots[0] is the insertion point -- so the candidate is the slot BEFORE
	 * it.
	 *
	 * slots[0] == 0 there means nothing in the tree precedes the key at all.
	 * Near-unreachable for a real file, since its (ino, INODE_ITEM, 0) sorts
	 * ahead of every EXTENT_DATA it owns -- but it is kept because the
	 * alternative is indexing slot -1 and decoding arbitrary leaf bytes.
	 *
	 * Note what is NOT needed: any backward walk.  btree_search's descent
	 * already steps to the child BEFORE the first separator greater than the
	 * key, so it lands in the leaf holding the predecessor even when that is
	 * a different leaf from the successor.  Forward scans need
	 * btree_next_leaf; containment lookups do not.
	 */
	if (s == 1) {
		if (path.slots[0] == 0) {
			goto out_hole;       /* nothing precedes it */
		}
		path.slots[0]--;
	}

	block = path.nodes[0]->b_data;
	slot  = path.slots[0];
	it    = bitter_leaf_item(block, (bt_u32) slot);
	bitter_key_from_disk(&leaf_key, &it->key);

	/*
	 * A HOLE, not corruption.  Stepping back can land on a different object,
	 * or on this file's own inode item -- type 1 sorts before type 96, so a
	 * file whose first extent starts past byte 0 lands exactly there.  Either
	 * way, no extent precedes this offset.
	 */
	if (leaf_key.objectid != ino || leaf_key.type != BITTER_EXTENT_DATA) {
		goto out_hole;
	}

	/* Before any payload field is read. */
	size = bt_get_le32(&it->size);
	if (size < BITTER_EXTENT_DATA_SIZE) {
		pr_err("inode %llu: extent item is %u bytes, under the %d-byte header\n",
		       (unsigned long long) ino, size, BITTER_EXTENT_DATA_SIZE);
		err = -EUCLEAN;
		goto out_unlock;
	}

	dat       = bitter_leaf_data(block, (bt_u32) slot);
	num_bytes = bt_get_le64(&dat->num_bytes);

	/*
	 * Also a hole: the nearest preceding item is not necessarily a COVERING
	 * one.  A gap between two extents lands here, which is what a sparse
	 * file is.
	 */
	if (file_offset >= leaf_key.offset + num_bytes) {
		goto out_hole;
	}

	/*
	 * Tested FOR the type we handle rather than against the ones we do not.
	 * A disk-supplied value of 0, or 7, or 200 must not fall through into
	 * the regular path and have disk_bytenr read out of an item that never
	 * had one set -- which is exactly why NONE is 0.
	 */
	if (dat->type == BITTER_FILE_EXTENT_INLINE) {
		/* Well-formed, and inexpressible as a block mapping: its bytes
		 * are inside the item.  Unimplemented rather than corrupt, the
		 * same call fill_super makes for an unsupported csum_type. */
		err = -EOPNOTSUPP;
		goto out_unlock;
	}
	if (dat->type != BITTER_FILE_EXTENT_REG) {
		pr_err("inode %llu: extent type %u\n",
		       (unsigned long long) ino, dat->type);
		err = -EUCLEAN;
		goto out_unlock;
	}

	disk_bytenr = bt_get_le64(&dat->disk_bytenr);

	/* The conventional spelling of an explicit hole: an item for the range
	 * with nothing backing it.  Address 0 is the reserved region anyway. */
	if (disk_bytenr == 0) {
		goto out_hole;
	}

	/*
	 * The three coordinate systems meeting:
	 *
	 *   disk_bytenr            where the extent begins on the device
	 * + dat->offset            where THIS FILE's data begins inside it
	 * + (file_offset - key)    how far into this item we are
	 */
	ext_offset  = bt_get_le64(&dat->offset);
	device_byte = disk_bytenr + ext_offset + (file_offset - leaf_key.offset);

	/* See the note at the top: this is the only disk-derived value in the
	 * module that reaches userspace with no checksum behind it. */
	if ((device_byte % BITTER_BLOCK_SIZE) != 0 ||
	    device_byte >= mnt->fs.total_bytes) {
		pr_err("inode %llu: extent maps to %#llx, outside the device or unaligned\n",
		       (unsigned long long) ino, (unsigned long long) device_byte);
		err = -EUCLEAN;
		goto out_unlock;
	}

	bitter_path_release(&mnt->env, &path);
	mutex_unlock(&mnt->lock);

	/* Four assignments to a struct: no sleeping, no I/O, nothing that could
	 * re-enter us.  Outside the lock for the shortest hold, not because it
	 * must be. */
	map_bh(bh_result, inode->i_sb, device_byte / BITTER_BLOCK_SIZE);
	return 0;

	/*
	 * Nothing backs this range, reached four ways: nothing precedes the key,
	 * the predecessor belongs to another object, a gap between two extents,
	 * or an explicit zero disk_bytenr.  All four are the same conclusion and
	 * a write answers all four identically.
	 *
	 * The path is released BEFORE allocating.  btree_insert re-searches with
	 * cow=1, and holding buffers that the CoW descent is about to copy and
	 * relink is a bug this project has had once already -- see docs/LOG.md,
	 * "btree_cow_block released a buffer the path still held".
	 */
out_hole:
	bitter_path_release(&mnt->env, &path);
	err = create ? bitterfs_get_block_create(inode, file_offset, bh_result) : 0;
	mutex_unlock(&mnt->lock);
	return err;

out_unlock:
	bitter_path_release(&mnt->env, &path);
	mutex_unlock(&mnt->lock);
	return err;
}

/*
 * Record that one block of `inode` starting at `file_offset` lives at
 * `disk_bytenr`, by inserting an EXTENT_DATA item into the FS tree.
 *
 * The first INSERT the kernel makes.  Everything before it was a lookup or an
 * in-place overwrite of a fixed-size item, so this is the first time the tree
 * can grow under a kernel caller -- btree_insert splits whatever is full on
 * the way down, dirties every block it touched, repairs the parent separator
 * when the item lands at slot 0, and releases the path.  None of that is ours,
 * which is why there is no dirty_block here and why update_inode needed one.
 *
 * Caller holds mnt->lock and an open transaction.
 *
 * -EEXIST means an extent already covers this offset: the file is being
 * overwritten, which under pure CoW is a different operation -- allocate a new
 * block, repoint the existing item, and record a -1 for the old one.  Returned
 * rather than asserted, because the culprit could be get_block's create branch
 * OR a tree that disagrees with itself.
 */
static int bitterfs_insert_file_extent(struct bitter_trans *trans,
				       struct inode *inode,
				       bt_u64 file_offset, bt_u64 disk_bytenr)
{
  struct bitterfs_mount	   *mnt = bitterfs_sb(inode->i_sb);
  struct bitter_env* env = &mnt->env;
  bt_u64			    ino = inode->i_ino;

  struct bitter_path	    path;
  struct bitter_key_cpu	    key;
  struct bitter_extent_data dat;

  int s;

  lockdep_assert_held(&mnt->lock);

  bitter_file_extent_key(&key, ino, file_offset);
  bitter_path_init(&path);

  /* Through the accessors, not by assignment: bt_le64 and bt_u64 are the same
   * typedef, so a direct store compiles, writes host order, and is wrong only
   * on a machine nobody here owns.  `type` is one byte and has no order. */
  bt_put_le64(&dat.disk_bytenr,	   disk_bytenr);
  bt_put_le64(&dat.disk_num_bytes, BITTER_BLOCK_SIZE);
  bt_put_le64(&dat.offset,	   0);
  bt_put_le64(&dat.num_bytes,	   BITTER_BLOCK_SIZE);
  dat.type = BITTER_FILE_EXTENT_REG;

  /* disk_num_bytes is the WHOLE extent and offset is where this file starts
   * inside it -- 4096 and 0 while every extent is one block owned outright.
   * Both stop being constants at the first reflink or partial overwrite. */

  s = btree_insert(env, &mnt->fs.fs_root, &key, &path, &dat,
		   BITTER_EXTENT_DATA_SIZE, trans);

  /* btree_insert released the path on every path out, including its own error
   * returns.  Releasing again is safe but is not owed. */
  return s;
}


static int bitterfs_get_block_create(struct inode *inode, bt_u64 file_offset,
                                     struct buffer_head *bh_result) {
  
  struct bitterfs_mount *mnt = bitterfs_sb(inode->i_sb);
  struct bitter_root* fs_root = &mnt->fs.fs_root;
  struct bitter_trans* trans;

  bt_u64 out;

  int s;
  int err;

  lockdep_assert_held(&mnt->lock);

  trans = bitterfs_trans_begin(mnt);
  if (IS_ERR(trans)) {
    return PTR_ERR(trans);  
  }

  s = bitter_alloc_data_block(fs_root, trans, &out);
  if (s < 0) {
    err = s;
    goto trans_out;
  } 

  s = bitterfs_insert_file_extent(trans, inode, file_offset, out);
  if (s < 0) {
    err = s;
    goto trans_out;
  }

  /* st_blocks counts 512-byte units regardless of block size, which is what
   * iget divides nbytes by on the way in.  Nothing else tracks this: without
   * it nbytes goes to disk stale and du disagrees with ls.  Safe unlocked --
   * the VFS holds i_rwsem exclusive across the whole write. */
  inode->i_blocks += BITTER_BLOCK_SIZE / 512;

  /* set_buffer_new says the block was just allocated and nothing under it is
   * meaningful, so __block_write_begin zeroes the parts this write does not
   * cover and drops any stale page-cache alias.  Without it a short write
   * leaks whatever a deleted file left in the block -- and reads back
   * correctly over the range that WAS written, which is what hides it. */
  map_bh(bh_result, inode->i_sb, out / BITTER_BLOCK_SIZE);
  set_buffer_new(bh_result);
  
  return bitterfs_trans_end(mnt);
   
trans_out:
  bitterfs_trans_end(mnt);
  return err; 
  
}

/*
 * Free the extents past `new_size`, up to as many as this transaction can
 * still record.  *done says whether the file is fully trimmed.
 *
 * --- why it is bounded ----------------------------------------------------
 * Every block freed records a -1 delayed ref, and the set holds
 * BITTERFS_DELAYED_REFS entries.  One block per extent, so a truncate of
 * anything past about 15 MB would overflow it -- and core/trans.h is explicit
 * that a full ref set is the one -BITTER_ENOMEM that cannot be backed out of,
 * because a change that is never recorded becomes a block handed to two owners
 * after the next mount.
 *
 * So this stops at the same threshold bitterfs_trans_end uses to decide
 * whether to commit.  That makes the caller's loop self-regulating: the helper
 * stops because the set is nearly full, and the trans_end that follows commits
 * for exactly that reason.
 *
 * --- why it deletes from the END ------------------------------------------
 * btree_del_item does its own search and releases the path before returning
 * (core/btree.h), so no position can be held across a delete.  Deleting the
 * LAST item each time means nothing after it shifts, and every iteration is
 * independent: search, decide, delete, repeat.
 *
 * The cost is a search per extent rather than a walk -- the same O(n log n)
 * readdir pays, and recorded in the same docs/LOG.md entry.
 *
 * --- why there is no partial-extent case ----------------------------------
 * An extent covers a whole block even when the file ends inside it, because
 * i_size is what bounds a read -- get_block checks it before it searches.  So
 * an extent straddling new_size needs no adjustment at all, and the two cases
 * are "entirely past, delete" and "stop".  The partial folio's tail is zeroed
 * by truncate_setsize in the caller, not here.
 *
 * Caller holds mnt->lock and an open transaction.
 */
int bitterfs_truncate_extents(struct bitter_trans* trans,
                          struct inode* inode, bt_u64 new_size, bool *done)
{
  struct bitterfs_mount *mnt = bitterfs_sb(inode->i_sb);
  struct bitter_env* env = &mnt->env;
  struct bitter_root* fs_root = &mnt->fs.fs_root;

  bt_u64 ino = inode->i_ino;

  struct bitter_key_cpu probe;
  struct bitter_key_cpu leaf_key;
  struct bitter_path path;
  struct bitter_item *it;
  struct bitter_extent_data *dat;

  bt_u64 disk_bytenr;
  bt_u8  type;
  bt_u32 size;
  void  *buf;
  bt_u32 slot;
  int    err;
  int    s;

  lockdep_assert_held(&mnt->lock);

  *done = false;

  /* A key nothing can match, so the descent takes the rightmost child at every
   * level and lands past this inode's last extent.  0xFF and ~0ULL for the
   * same reason bitter_find_last_objectid uses them: type dominates offset, so
   * a lower type would sort BEFORE the items being looked for. */
  probe.objectid = ino;
  probe.type     = BITTER_EXTENT_DATA;
  probe.offset   = ~0ULL;

  for (;;) {
    /*
     * Stop before the set is full rather than after.  The MARGIN, not the
     * capacity, because trans_add_delayed_ref can still fail on the very next
     * call and that failure is the unrecoverable one.
     */
    if (trans->ref_count >= BITTERFS_DELAYED_REFS - BITTERFS_DELAYED_MARGIN) {
      return 0;                 /* *done stays false: more to do */
    }

    bitter_path_init(&path);

    /* trans NULL and cow 0: this search only reads.  The write happens in
     * btree_del_item, which does its own CoW descent. */
    s = btree_search(env, fs_root, &probe, &path, 0, NULL, 0);
    if (s < 0) {
      return s;                 /* search released the path itself */
    }

    /*
     * The probe cannot match, so s is always 1 and slots[0] is the insertion
     * point -- one PAST the last item.  Step back to reach it.
     *
     * slots[0] == 0 means nothing in the tree precedes the probe at all, which
     * for a key this high means an empty tree.  Not reachable for a real file,
     * whose inode item sorts ahead of every extent it owns, but the
     * alternative is indexing slot -1.
     */
    if (path.slots[0] == 0) {
      err = 0;
      *done = true;
      goto out_release;
    }
    slot = (bt_u32)(path.slots[0] - 1);

    buf = path.nodes[0]->b_data;
    it  = bitter_leaf_item(buf, slot);
    bitter_key_from_disk(&leaf_key, &it->key);

    /* Stepped back off this inode's range entirely: no extents left. */
    if (leaf_key.objectid != ino || leaf_key.type != BITTER_EXTENT_DATA) {
      err = 0;
      *done = true;
      goto out_release;
    }

    /*
     * The only test that matters.  An extent starting before new_size is
     * either the one the file now ends inside, or entirely before it -- and
     * neither is freed.  Since the scan runs backwards, the first such extent
     * means every remaining one is too.
     */
    if (leaf_key.offset < new_size) {
      err = 0;
      *done = true;
      goto out_release;
    }

    /* Before any payload byte is read. */
    size = bt_get_le32(&it->size);
    if (size < BITTER_EXTENT_DATA_SIZE) {
      pr_err("inode %llu: extent at %llu is under the %d-byte header\n",
             (unsigned long long) ino, (unsigned long long) leaf_key.offset,
             BITTER_EXTENT_DATA_SIZE);
      err = -EUCLEAN;
      goto out_release;
    }

    /*
     * Copied out before the release: both point into the leaf's buffer, and
     * btree_del_item below will CoW the blocks this path is holding.
     */
    dat         = bitter_leaf_data(buf, slot);
    type        = dat->type;
    disk_bytenr = bt_get_le64(&dat->disk_bytenr);

    bitter_path_release(env, &path);

    s = btree_del_item(env, fs_root, &leaf_key, &path, trans);
    if (s < 0) {
      return s;                 /* del_item released the path itself */
    }

    /*
     * Only a REG extent with a real address owns a block.
     *
     *   INLINE      its bytes were inside the item, so deleting the item IS
     *               freeing them, and disk_bytenr was never set.
     *   bytenr 0    an explicit hole: an item for the range with nothing
     *               behind it.  Address 0 is the reserved region.
     *
     * Scheduling a -1 for either would decrement a block that was never
     * allocated -- the one mistake here that corrupts rather than fails.
     */
    if (type != BITTER_FILE_EXTENT_REG || disk_bytenr == 0) {
      continue;
    }

    /*
     * BITTER_BLOCK_SIZE, matching what bitter_alloc_data_block recorded.
     * core/trans.h merges on an EXACT match of bytenr AND length, because an
     * extent item is keyed (bytenr, EXTENT_ITEM, length) -- a different length
     * names a different item.  The +1 and the -1 must never disagree.
     *
     * After the delete, not before: if this fails the block leaks, which a
     * rescan recovers.  The other order would schedule a free for a block the
     * tree still points at.
     */
    s = trans_add_delayed_ref(trans, disk_bytenr, BITTER_BLOCK_SIZE, -1);
    if (s < 0) {
      return s;
    }
  }

out_release:
  bitter_path_release(env, &path);
  return err;
}

/*
 * Point this file's extent at a freshly allocated block, and release the old
 * one.  The new address comes back through `out`.
 *
 * The overwrite half of the write path, and the sibling of
 * bitterfs_insert_file_extent: that one handles s == 1 (no item here), this one
 * s == 0 (an item already covers it).  An in-place payload overwrite -- same
 * key, same 33 bytes, same slot -- so it is bitterfs_update_inode's shape, not
 * an insert's, and nothing in the leaf moves.
 *
 * Why it exists at all: without it an overwrite lands on the block the item
 * already names, which is in-place data modification and not copy-on-write.
 * The metadata trees would still be pure CoW and the data would not, which is
 * exactly the property snapshots depend on.
 *
 * Caller holds mnt->lock and an open transaction.
 */
static int bitterfs_cow_file_extent(struct bitter_trans* trans,
                                                   struct inode* inode,
                                                   bt_u64 file_offset,
                                                   bt_u64* out) {

  struct bitterfs_mount* mnt = bitterfs_sb(inode->i_sb);
  struct bitter_root* fs_root = &mnt->fs.fs_root;
  struct bitter_env* env = &mnt->env;

  struct bitter_path path;
  struct bitter_key_cpu key;

  bt_u64 ino = inode->i_ino;
  bt_u64 newaddr = 0;
  bt_u64 oldaddr;

  int err;
  int s;
  bt_u32 slot;

  void* buf;
  struct bitter_item* it;
  struct bitter_extent_data* dat;

  lockdep_assert_held(&mnt->lock);

  bitter_file_extent_key(&key, ino, file_offset);

  /* Mandatory before first use: btree_search releases the path on entry, so an
   * uninitialised stack path means put_block on whatever was in this frame. */
  bitter_path_init(&path);

  /* trans non-null and cow 1 -- the pair that makes this a write.  ins_len 0
   * because the replacement is the same 33 bytes under the same key. */
  s = btree_search(env, fs_root, &key, &path, 0, trans, 1);

  if (s < 0) {
    /* btree_search released the path on its own error returns. */
    return s;
  }
  /*
   * No item starts exactly here.  Unreachable today: the caller only CoWs
   * buffers get_block already mapped, so an extent covers this offset -- and
   * every extent is exactly one block at a block-aligned key, so "covers" and
   * "starts at" coincide.
   *
   * That equivalence is the assumption.  With multi-block extents this becomes
   * a legitimate case meaning "an extent covers this offset but starts
   * earlier, so split it" -- three items where there was one, and the `offset`
   * and `disk_num_bytes` fields stop being constants.
   */
  if (s != 0) {
    err = -EUCLEAN;
    goto fail_out;
  }

  buf = path.nodes[0]->b_data;
  slot = (bt_u32) path.slots[0];

  it = bitter_leaf_item(buf, slot);
  dat = bitter_leaf_data(buf, slot);

  /*
   * Before any payload byte is read OR written, and the write is why this
   * matters more here than on the read path: bt_put_le64 over an item too
   * short to hold eight bytes lands in the neighbour's payload, and every leaf
   * descriptor stays consistent afterwards -- bitter_leaf_check passes and
   * fsck finds nothing.
   *
   * `<` rather than `!=`, matching get_block: an EXTENT_DATA item is allowed
   * to be longer than its header, which is what an inline extent is.
   */
  if (bitter_item_get_size(it) < BITTER_EXTENT_DATA_SIZE) {
    pr_err("inode %llu: extent at %llu is under the %d-byte header\n",
           (unsigned long long) ino, (unsigned long long) file_offset,
           BITTER_EXTENT_DATA_SIZE);
    err = -EUCLEAN;
    goto fail_out;
  }

  /*
   * Tested FOR the type we handle, the same three-way split get_block uses.
   * An inline extent has no disk_bytenr to free, so CoWing one would schedule
   * a -1 against whatever garbage sat in those eight bytes -- a refcount drop
   * on a block that was never allocated.
   */
  if (dat->type == BITTER_FILE_EXTENT_INLINE) {
    err = -EOPNOTSUPP;
    goto fail_out;
  }

  if (dat->type != BITTER_FILE_EXTENT_REG) {
    pr_err("inode %llu: extent at %llu has type %u, cannot CoW\n",
           (unsigned long long) ino, (unsigned long long) file_offset,
           dat->type);
    err = -EUCLEAN;
    goto fail_out;
  }

  /* READ BEFORE OVERWRITING.  The item is the only record of the old address,
   * and the bt_put_le64 below destroys it. */
  oldaddr = bt_get_le64(&dat->disk_bytenr);

  s = bitter_alloc_data_block(fs_root, trans, &newaddr);
  if (s < 0) {
    err = s;
    goto fail_out;
  }

  bt_put_le64(&dat->disk_bytenr, newaddr);

  /*
   * The first negative delayed ref in the filesystem; everything until now has
   * been +1 from the two allocators.  Recorded AFTER the item stops pointing at
   * it, so there is no window where the tree names a block marked for freeing.
   *
   * The block does not return to the free map now -- that is rebuilt from the
   * extent tree at the next mount -- so a long-lived mount that overwrites
   * repeatedly consumes fresh blocks and reclaims none until unmount.
   *
   * Checked, unlike the +1 sites' habit, because a full ref set is the one
   * -BITTER_ENOMEM that cannot be backed out of: the item already names the new
   * block, so failing to record the old one leaks it permanently.
   */
  s = trans_add_delayed_ref(trans, oldaddr, BITTER_BLOCK_SIZE, -1);
  if (s < 0) {
    err = s;
    goto fail_out;
  }

  /* Ours to dirty: nothing else knows the payload changed.  The CoW descent
   * already dirtied this leaf, but bitterfs_dirty_block recomputes the checksum
   * and then hands the buffer to writeback -- that earlier checksum covers the
   * bytes as they were BEFORE this edit.
   *
   * Before the release, which is brelse: dirty_block needs the buffer head. */
  env->ops->dirty_block(env, path.nodes[0]);
  bitter_path_release(env, &path);

  *out = newaddr;
  return 0;

fail_out:
  bitter_path_release(env, &path);
  return err;
}

/*
 * One line, because everything hard is borrowed: mpage_read_folio owns the
 * folio locking, the bio submission, and the zero-filling of any range whose
 * buffer head came back unmapped.
 */
static int bitterfs_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, bitterfs_get_block);
}

static void bitterfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, bitterfs_get_block);
}

/*
 * Write every dirty folio of this mapping back to the device.
 *
 * NOT optional, and its absence is silent.  do_writepages (mm/page-writeback.c)
 * falls through to `ret = 0` when a mapping has neither ->writepages nor
 * ->writepage -- success, having written nothing.  The data would sit dirty in
 * the page cache until unmount dropped it.
 *
 * sync_blockdev does not cover this.  That syncs the BLOCK DEVICE's mapping,
 * where env_kernel.c's metadata buffer heads live; file data is in the inode's
 * own mapping and only this reaches it.
 *
 * mpage_writepages does the folio walk, the buffer-head lookup and the bio
 * building; bitterfs_get_block supplies the mapping, with create == 0 because
 * by writeback time every block already has one.
 */
static int bitterfs_writepages(struct address_space *mapping,
			       struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, bitterfs_get_block);
}

/*
 * Get the target folio locked and every block under it mapped -- then make the
 * mapping copy-on-write.
 *
 * block_write_begin does the first half: it allocates for any block with no
 * extent (through get_block's create branch), reads the old contents for a
 * partial write, and hands back a locked, uptodate folio.
 *
 * The second half is why this is not a one-liner like read_folio.  At that
 * point every buffer that ALREADY had an extent is mapped to the block that
 * extent names, and writing there would modify data in place -- fine for a
 * conventional filesystem, and not what bitterfs is.  So each such buffer is
 * repointed at a fresh block and the old one is scheduled for release.
 *
 * It has to happen HERE, after block_write_begin and not inside get_block,
 * and the reason is in __block_write_begin (fs/buffer.c): for a partial write
 * it either zeroes the untouched bytes (when the buffer is new) or READS them
 * from whatever the buffer is mapped to.  Map a fresh block during get_block
 * and one of those two is always wrong -- the old bytes are lost, or garbage is
 * read in their place.  Letting the read happen from the OLD block first, and
 * only then remapping, is what makes the folio carry the right contents to the
 * new location.
 *
 * LOCKING: mnt->lock is taken only after block_write_begin returns.  Wrapping
 * the call would self-deadlock -- get_block takes the same mutex internally.
 * The folio lock is held throughout, which is the order the rest of the module
 * already uses (mpage_read_folio and mpage_writepages both lock the folio and
 * then call get_block).
 */
static int bitterfs_write_begin(struct file* file, struct address_space *mapping,
                                loff_t pos, unsigned len,
                                struct folio **foliop, void **fsdata) {

  struct inode* inode = mapping->host;
  struct bitterfs_mount* mnt = bitterfs_sb(inode->i_sb);
  struct bitter_trans* trans;

  struct folio* folio;
  struct buffer_head* bh;
  struct buffer_head* head;

  unsigned from;
  unsigned to;
  unsigned block_start;
  unsigned block_end;

  bt_u64 newaddr;
  int err;
  int r;

  err = block_write_begin(mapping, pos, len, foliop, bitterfs_get_block);
  if (err) {
    /* Nothing to unwind: on failure it unlocks and drops the folio itself. */
    return err;
  }

  /* From here the folio is LOCKED and held, and every exit owes folio_unlock
   * plus folio_put -- generic_perform_write does not do it for us. */
  folio = *foliop;

  from = offset_in_folio(folio, pos);
  to   = from + len;
  if (to > folio_size(folio)) {
    to = folio_size(folio);
  }

  head = folio_buffers(folio);
  BITTER_ASSERT(head);

  mutex_lock(&mnt->lock);

  trans = bitterfs_trans_begin(mnt);
  if (IS_ERR(trans)) {
    err = PTR_ERR(trans);
    goto out_unlock;      /* joined nothing, so no trans_end */
  }

  /*
   * The circular buffer walk from __block_write_begin.  One folio is one block
   * here (get_block asserts it), so this runs once -- written as a loop anyway,
   * because the day that assertion is relaxed this is the code that has to
   * already be right.
   */
  for (bh = head, block_start = 0; bh != head || !block_start;
       block_start = block_end, bh = bh->b_this_page) {

    block_end = block_start + bh->b_size;

    /* Outside the range being written: untouched, so its mapping stands. */
    if (block_end <= from || block_start >= to) {
      continue;
    }

    /*
     * buffer_new means get_block_create allocated this block moments ago --
     * already fresh, already recorded, and CoWing it would allocate a second
     * and lose the first.
     *
     * Not airtight, and the gap is worth naming: __block_write_begin clears
     * the flag when the folio was ALREADY uptodate, which happens when a hole
     * in a cached folio is written.  Such a block is then CoW'd needlessly --
     * the +1 and -1 cancel inside one transaction (see core/trans.h) so
     * nothing leaks, but a free-map entry is spent that only the next mount's
     * rescan reclaims.  Wasteful, not wrong.
     */
    if (buffer_new(bh) || !buffer_mapped(bh)) {
      continue;
    }

    err = bitterfs_cow_file_extent(trans, inode,
                                   folio_pos(folio) + block_start, &newaddr);
    if (err) {
      break;
    }

    /* The item names the new block; without this the buffer still names the
     * old one, so writeback would write to an address just freed while the
     * tree points at one nothing ever wrote. */
    bh->b_blocknr = newaddr / BITTER_BLOCK_SIZE;
  }

  /* Unconditional, error path included: the only thing that decrements
   * trans_joiners. */
  r = bitterfs_trans_end(mnt);
  if (!err) {
    err = r;
  }

out_unlock:
  if (err) {
    bitterfs_write_failed(mnt, "extent CoW", err);
  }

  mutex_unlock(&mnt->lock);

  if (err) {
    folio_unlock(folio);
    folio_put(folio);
  }

  return err;
}

/*
 * Make this file durable on demand, rather than whenever the batching threshold
 * happens to fire.
 *
 * Two halves, and the order between them is the correctness argument rather
 * than tidiness: the data must be on the device BEFORE the commit publishes a
 * superblock whose extent items point at it.  Reversed, a power cut in between
 * leaves a valid tree naming blocks that still hold whatever was there before
 * -- every checksum intact, every invariant satisfied, the contents somebody
 * else's.
 *
 * `datasync` is ignored.  fdatasync(2) may skip metadata not needed to retrieve
 * the data -- a bare mtime change -- but bitterfs has ONE transaction covering
 * the whole filesystem and no way to commit part of it.  The stricter
 * behaviour is always correct, just not always cheap.
 *
 * Which is this function's real cost: it commits every pending change from
 * every file, so one process's fsync pays for another's writes.  Filesystems
 * that avoid that keep per-inode logs; bitterfs has nowhere to put one.
 *
 * Shared with directories -- see bitterfs_dir_operations.  fsync(dirfd) is how
 * a program makes a NAME durable, and without a member there vfs_fsync_range
 * returns -EINVAL to a program doing exactly the right thing.
 */
int bitterfs_fsync(struct file* file, loff_t start, loff_t end, int datasync) {
  struct inode *inode = file->f_mapping->host;
  struct bitterfs_mount *mnt = bitterfs_sb(inode->i_sb);
  int err;

  err = file_write_and_wait_range(file, start, end);
  if (err < 0) {
    return err;
  }
  mutex_lock(&mnt->lock);
  err = bitterfs_commit(mnt);
  mutex_unlock(&mnt->lock);
  return err;
}

/*
 * Accept the bytes that were copied, and make the size change durable.
 *
 * generic_write_end does nearly all of it -- marks the buffers dirty, grows
 * i_size, unlocks and puts the folio -- and returns the byte count, which is
 * this function's return value on success.
 *
 * What it cannot do is persist anything.  It ends in mark_inode_dirty, which
 * queues the inode for a ->write_inode that bitterfs does not have, so the
 * i_size it just set would live only in memory and the file would read back at
 * its old length after a remount.  That is the whole reason this is not a
 * one-liner like write_begin.
 *
 * Unconditional rather than gated on i_size changing: generic_file_write_iter
 * has already moved mtime and ctime, and get_block_create may have moved
 * i_blocks, so there is nearly always something to write even when the file
 * did not grow.
 *
 * ORDER: after generic_write_end, never before.  It unlocks the folio on its
 * way out, and starting a transaction under the folio lock is exactly the lock
 * ordering its own source warns about.
 */
static int bitterfs_write_end(struct file *file, struct address_space *mapping,
                            loff_t pos, unsigned len, unsigned copied,
                            struct folio *folio, void *fsdata) {

  struct inode		*inode = mapping->host;
  struct bitterfs_mount	*mnt   = bitterfs_sb(inode->i_sb);
  struct bitter_trans	*trans;

  int ret;
  int err;
  int r;

  ret = generic_write_end(file, mapping, pos, len, copied, folio, fsdata);

  mutex_lock(&mnt->lock);

  trans = bitterfs_trans_begin(mnt);
  if (IS_ERR(trans)) {
    /* Joined nothing, so no trans_end -- the one exit that does not owe it. */
    err = PTR_ERR(trans);
    goto out_unlock;
  }

  err = bitterfs_update_inode(trans, inode);

  /* Unconditional, error path included: trans_end is the only thing that
   * decrements trans_joiners. */
  r = bitterfs_trans_end(mnt);
  if (!err)
    err = r;

out_unlock:
  if (err)
    bitterfs_write_failed(mnt, "inode update", err);

  mutex_unlock(&mnt->lock);

  /*
   * The error wins over the byte count.  The bytes really are in the page
   * cache, so this reports a write that partly happened -- but the alternative
   * is returning success for a size that never reached the disk, which is the
   * failure this function exists to prevent.  generic_perform_write reverts
   * the iterator and stops the loop.
   */
  return err ? err : ret;
}

/*
 * .readahead is the batched form of read_folio; it is what turns a sequential
 * read from one folio at a time into one bio.
 *
 * --- the three that are easy to omit --------------------------------------
 * .write_begin and .write_end are the obvious pair.  The others are not, and
 * each fails quietly rather than loudly:
 *
 *   .writepages      without it do_writepages returns 0 having written
 *                    nothing, and every buffered write is lost at unmount.
 *   .dirty_folio     the default, filemap_dirty_folio, knows nothing about
 *                    buffer heads, so the buffers under a dirtied folio are
 *                    never marked and writeback skips them.
 *   .invalidate_folio  releases the buffer heads when a folio is dropped.
 *                    Without it they leak.
 *
 * All three are borrowed verbatim from fs/buffer.c, and ext2's table
 * (fs/ext2/inode.c) carries the same three for the same reasons.
 *
 * Still absent, deliberately: .bmap (FIBMAP, and swapfiles, neither of which
 * bitterfs supports), .migrate_folio, .is_partially_uptodate.  Those are
 * optimisations, not correctness.
 */
const struct address_space_operations bitterfs_aops = {
	.read_folio		= bitterfs_read_folio,
	.readahead		= bitterfs_readahead,
	.write_begin		= bitterfs_write_begin,
	.write_end		= bitterfs_write_end,
	.writepages		= bitterfs_writepages,
	.dirty_folio		= block_dirty_folio,
	.invalidate_folio	= block_invalidate_folio,
};

/*
 * Two borrowed implementations, and nothing filesystem-specific.  Everything
 * particular to bitterfs about reading a file lives in read_folio, which does
 * not exist yet -- so this table is entirely a statement about WHICH generic
 * paths apply.
 *
 * .read is deliberately absent: with .read_iter set, vfs_read wraps it through
 * new_sync_read, so read(2) works without a second entry point.
 *
 * .mmap and .splice_read are absent for a harder reason -- both go straight to
 * read_folio on the first fault, so adding them before it exists would turn a
 * clean -ENODEV into a fault that never resolves.  They arrive together.
 *
 * No inode_operations for regular files: ->getattr being NULL makes stat use
 * generic_fillattr straight from the inode, which is right, since every field
 * it wants is already there.
 */


const struct file_operations bitterfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,

	/* The entry point for write(2), and without it nothing in the aops
	 * above is reachable: generic_file_write_iter is what runs the
	 * write_begin / copy / write_end loop.  .write is absent for the same
	 * reason .read is -- vfs_write wraps write_iter through
	 * new_sync_write. */
	.write_iter	= generic_file_write_iter,

	.fsync		= bitterfs_fsync,
};

/*
 * Regular files had no inode_operations at all until now: everything stat
 * wants was already in the inode, so the VFS defaults were right.
 *
 * ->setattr is the first thing that cannot be defaulted -- without a table
 * here, chmod on a file would return -EPERM while chmod on a directory
 * worked.  Still no .getattr, for the original reason.
 */
const struct inode_operations bitterfs_file_inode_operations = {
	.setattr = bitterfs_setattr,
};

