// SPDX-License-Identifier: GPL-2.0
/*
 * bitterfs — the kernel's half of the seam.
 *
 * core/bitter_env.h names six operations core/ needs from the world outside
 * it; this file supplies them over the buffer cache, exactly as
 * user/env_user.c supplies them over pread and malloc.  Same core/, two
 * environments -- which is what lets a B-tree bug be found by a userspace test
 * in thirty seconds rather than by a guest panic.
 *
 * The two are not identical in behaviour, and one difference matters: the
 * buffer cache MEANS it.  Two read_block calls for the same bytenr here return
 * the same buffer_head and the same b_data, while env_user (deliberately dumb,
 * no cache) hands back two independent copies.  core/ must be correct either
 * way, assuming neither aliasing nor independence.
 */

/* Before every kernel header -- printk.h expands pr_*() with whatever pr_fmt
 * is in scope at that point.  Without this the messages below reach dmesg with
 * no "bitterfs: " on them, and qemu/guest.sh greps for exactly that. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/bitter_endian.h"
#include "core/bitter_crc32c.h"
#include "core/bitter_assert.h"
#include "kernel/bitterfs.h"

/*
 * Fetch an existing block, verify it is the block it claims to be, and hand
 * core/ a bitter_buf for it.  NULL on any failure.
 *
 * NULL is the whole error channel -- the signature is bitter_env_ops's and has
 * no room for a code -- so every refusal below says why in dmesg first.
 */
static struct bitter_buf *bitterfs_read_block(struct bitter_env *env,
					      bt_u64 bytenr)
{
	struct bitterfs_mount	*mnt = env->priv;
	struct buffer_head	*bh;
	struct bitter_header	*hdr;
	struct bitter_buf	*buf;
	bt_u32			 want, have;
	bt_u64			 claimed;

	/* Whose fault would this be?  A misaligned address reached us from
	 * core/, so ours -- core/bitter_assert.h's test says assert. */
	BITTER_ASSERT(bytenr % BITTER_BLOCK_SIZE == 0);

	bh = sb_bread(mnt->sb, bytenr / BITTER_BLOCK_SIZE);
	if (!bh) {
		/* The one failure path that owes no brelse: __bread_slow
		 * released the buffer before returning NULL. */
		pr_err("read_block: %#llx: read failed\n",
		       (unsigned long long)bytenr);
		return NULL;
	}

	hdr = (struct bitter_header *)bh->b_data;

	/*
	 * The three self-identifying checks, in env_user.c's order and for its
	 * reason: each presupposes the one before.  The bytes are intact, then
	 * it is the block we asked for, then it belongs to this filesystem.
	 */
	want = bt_get_le32(hdr->csum);
	have = bt_block_csum(bh->b_data, BITTER_BLOCK_SIZE);
	if (want != have) {
		pr_err("read_block: %#llx: csum %08x, computed %08x\n",
		       (unsigned long long)bytenr, want, have);
		brelse(bh);
		return NULL;
	}

	/* Catches a misdirected write: intact, correctly checksummed, and in
	 * the wrong place. */
	claimed = bt_get_le64(&hdr->bytenr);
	if (claimed != bytenr) {
		pr_err("read_block: %#llx: block claims %#llx\n",
		       (unsigned long long)bytenr,
		       (unsigned long long)claimed);
		brelse(bh);
		return NULL;
	}

	/*
	 * No have_fsid gate, unlike env_user.  Every read here happens after
	 * fill_super read the superblock and filled mnt->fsid, because
	 * fill_super is what installs this vtable in the first place; env_user
	 * needs its gate for mkfs and the unit fixtures, which use the seam
	 * with no superblock at all.
	 *
	 * That makes the invariant fill_super's to keep -- see the ordering
	 * note in kernel/bitterfs.h.  An unarmed fsid is sixteen zero bytes,
	 * which would silently match a zeroed block rather than fail loudly.
	 */
	if (memcmp(hdr->fsid, mnt->fsid, BITTER_FSID_SIZE) != 0) {
		pr_err("read_block: %#llx: fsid %pUb, expected %pUb\n",
		       (unsigned long long)bytenr, hdr->fsid, mnt->fsid);
		brelse(bh);
		return NULL;
	}

	/*
	 * Last, so a block that fails a check costs no allocation -- env_user
	 * cannot do this, because it reads INTO its own storage.
	 *
	 * GFP_NOFS, not GFP_KERNEL: we are on the filesystem path, and
	 * GFP_KERNEL permits reclaim to call back into filesystem writeback and
	 * re-enter us.
	 */
	buf = kmalloc(sizeof(*buf), GFP_NOFS);
	if (!buf) {
		brelse(bh);
		return NULL;
	}

	/* b_data points INTO the page cache; copying would decouple what core/
	 * modifies from what gets written back.  b_priv carries the
	 * buffer_head that put_block must brelse. */
	buf->b_bytenr = bytenr;
	buf->b_data   = bh->b_data;
	buf->b_priv   = bh;

	return buf;
}

/*
 * Release.  Just a release, unlike env_user's, which is also the write path:
 * in the kernel a modified block reaches the disk through mark_buffer_dirty
 * and writeback, never through here.
 *
 * brelse BEFORE kfree -- reading buf->b_priv after freeing buf is a
 * use-after-free, and a loud one with KASAN on.
 *
 * brelse only drops OUR reference; the buffer stays in the page cache, so the
 * next read of this block costs no I/O.  `env` is unused because b_priv
 * carries everything needed.
 */
static void bitterfs_put_block(struct bitter_env *env, struct bitter_buf *buf)
{
	brelse(buf->b_priv);
	kfree(buf);
}

/*
 * --- the write half, which phase 5 never reaches -------------------------
 *
 * A read-only mount allocates nothing, dirties nothing and commits nothing, so
 * core/ arriving at any of these is a bug in the kernel module rather than
 * anything the disk did -- which is core/bitter_assert.h's test for when to
 * assert instead of returning.
 *
 * Each still returns afterwards, because BITTER_ASSERT compiles to nothing
 * without BITTER_DEBUG: in such a build these must fail rather than fall off
 * the end of a non-void function.
 *
 * Phase 7 replaces them, and until then the assertions mark exactly where the
 * work is.
 */
/*
 * A buffer for a block whose contents do not matter yet, because core/ is
 * about to overwrite all of it.
 *
 * sb_getblk, NOT sb_bread: reading a block that is about to be fully replaced
 * is a wasted I/O on every allocation, and avoiding it is the entire reason
 * this operation exists separately from read_block.  bdev_getblk's own comment
 * spells out the contract -- "the buffer may not be uptodate; the caller can
 * bring it uptodate either by reading it or overwriting it" -- and this is the
 * overwriting half.
 *
 * `bytenr` was chosen by bitter_alloc_block, taken from the free-space map,
 * and is already claimed.  The environment is being told the answer, not asked:
 * core/ decides WHAT, the environment does HOW.  core/cow.c then reads the
 * address back out of b_bytenr to stamp the block's own header, to write the
 * parent's pointer at it, and to free the block it replaced.
 */
static struct bitter_buf *bitterfs_alloc_block_buf(struct bitter_env *env,
						   bt_u64 bytenr)
{
	struct bitterfs_mount	*mnt = env->priv;
	struct buffer_head	*bh;
	struct bitter_header	*hdr;
	struct bitter_buf	*buf;

	/* A misaligned address came from core/, so it is our bug -- the same
	 * test read_block applies. */
	BITTER_ASSERT(bytenr % BITTER_BLOCK_SIZE == 0);

	bh = sb_getblk(mnt->sb, bytenr / BITTER_BLOCK_SIZE);
	if (!bh) {
		/* Near-unreachable: __getblk sets __GFP_NOFAIL, so the
		 * allocator loops rather than returning NULL.  Nothing to
		 * release -- there is no buffer to release. */
		pr_err("alloc_block_buf: %#llx: getblk failed\n",
		       (unsigned long long) bytenr);
		return NULL;
	}

	hdr = (struct bitter_header *) bh->b_data;

	/*
	 * Zeroed because EVERY BYTE is inside the checksum.  Uninitialised
	 * padding would produce a valid checksum over garbage, and the block
	 * would verify perfectly while containing whatever was in that page --
	 * which in the kernel is a previous block's contents, since sb_getblk
	 * may hand back a buffer the cache already had.
	 */
	memset(hdr, 0, BITTER_BLOCK_SIZE);

	/*
	 * The fsid is the environment's to stamp, not core/'s -- the same
	 * division as the checksum.  Which filesystem a block belongs to is a
	 * property of the environment that created it, and there is no fsid in
	 * bitter_root or bitter_trans for core/ to read.  bitter_alloc_block
	 * fills every other header field; this is the one it cannot.
	 *
	 * No have_fsid gate, unlike env_user: mkfs and the unit fixtures
	 * allocate blocks with no superblock read, and the kernel has no such
	 * caller.  fill_super stores the fsid before the vtable is reachable.
	 */
	memcpy(hdr->fsid, mnt->fsid, BITTER_FSID_SIZE);

	/*
	 * LAST of the content steps, and the barrier inside it is why: it
	 * guarantees a CPU that sees the flag also sees the bytes written
	 * above.
	 *
	 * Not optional.  mark_buffer_dirty opens with
	 * WARN_ON_ONCE(!buffer_uptodate(bh)), so without this the first write
	 * puts a stack trace in dmesg -- and writeback will not write a block it
	 * believes it has not read.
	 */
	set_buffer_uptodate(bh);

	buf = kmalloc(sizeof(*buf), GFP_NOFS);
	if (!buf) {
		/* Unlike read_block's NULL path, this one DOES owe a brelse:
		 * sb_bread releases the buffer itself before returning NULL,
		 * sb_getblk hands one back with the count elevated. */
		brelse(bh);
		return NULL;
	}

	/* b_data points INTO the page cache; copying would decouple what core/
	 * modifies from what gets written back.  b_priv carries the buffer_head
	 * that dirty_block and put_block both need. */
	buf->b_bytenr = bytenr;
	buf->b_data   = bh->b_data;
	buf->b_priv   = bh;

	return buf;
}

/*
 * "I changed this block."
 *
 * The checksum is computed HERE rather than at put_block, and that is the one
 * decision in this function.  mark_buffer_dirty hands the buffer to the
 * kernel's writeback machinery, which may write it at any moment afterwards
 * without passing through this module again -- so the checksum has to be
 * correct the instant that call returns.
 *
 * env_user computes it in put_block instead, because there put_block IS the
 * write path.  Its comment explains what that buys: "a block is modified many
 * times between the CoW that created it and the write that ends its life in
 * memory, and only the last of those states goes to disk."  Here every one of
 * those states costs a fresh crc32c over 4096 bytes, because none of them can
 * be assumed not to reach the device.  Recorded in docs/LOG.md.
 *
 * bt_block_csum takes the WHOLE block: it skips the leading BITTER_CSUM_SIZE
 * itself, because a field cannot be part of its own input.  And hdr->csum
 * takes no & -- it is a bt_u8 array, which decays, unlike the scalar fields
 * that need one.
 *
 * void, because marking a buffer dirty cannot fail.  A write that does fail
 * surfaces later, through writeback_all.
 */
static void bitterfs_dirty_block(struct bitter_env *env, struct bitter_buf *buf)
{
	struct buffer_head   *bh  = buf->b_priv;
	struct bitter_header *hdr = buf->b_data;

	bt_u32 csum = bt_block_csum(buf->b_data, BITTER_BLOCK_SIZE);

	bt_put_le32(hdr->csum, csum);

	/* After this line the buffer belongs to writeback. */
	mark_buffer_dirty(bh);
}

static int bitterfs_flush_device(struct bitter_env *env)
{
  struct bitterfs_mount* mnt = env->priv;

  return blkdev_issue_flush(mnt->sb->s_bdev);
}

static int bitterfs_writeback_all(struct bitter_env *env)
{
  struct bitterfs_mount* mnt = env->priv;

  return sync_blockdev(mnt->sb->s_bdev);
}

/*
 * One table for the whole module -- the function pointers never vary, only
 * priv does.  Designated initialisers so a forgotten member is NULL, which is
 * a predictable crash rather than a jump to whatever was in memory.
 */
/*
 * The superblock: the one block that cannot go through read_block.  It is
 * checksummed over BITTER_SUPER_SIZE rather than the whole block, and it is a
 * bitter_super rather than a bitter_header, so every check read_block makes
 * would be made against the wrong bytes.
 *
 * The same bare sb_bread fill_super uses, for the same reasons -- and block 16
 * because BITTER_SUPER_OFFSET divides exactly by BITTER_BLOCK_SIZE, so the
 * 512-byte superblock sits at the START of that block with no offset
 * arithmetic inside the buffer.
 *
 * Validates nothing: core/ owns what a valid superblock is.
 */
static int bitterfs_read_super(struct bitter_env *env, void *buf)
{
	struct bitterfs_mount *mnt = env->priv;
	struct buffer_head    *bh;

	bh = sb_bread(mnt->sb, BITTER_SUPER_OFFSET / BITTER_BLOCK_SIZE);
	if (!bh) {
		pr_err("read_super: cannot read block %d\n",
		       BITTER_SUPER_OFFSET / BITTER_BLOCK_SIZE);
		return -BITTER_EIO;
	}

	memcpy(buf, bh->b_data, BITTER_SUPER_SIZE);
	brelse(bh);
	return 0;
}

/*
 * Phase 7 and not before.  This is THE write -- the single overwrite that
 * makes a transaction real -- so it cannot be a mark_buffer_dirty that
 * writeback performs whenever it likes: the whole commit protocol is an
 * ordering, and an unordered superblock write breaks it in a way nothing
 * detects until a power cut.
 *
 * What it will need is the buffer written and ordered HERE, with durability
 * still left to trans_commit's second flush_device.
 */
static int bitterfs_write_super(struct bitter_env *env, const void *buf)
{
  struct buffer_head* bh;
  struct bitterfs_mount* mnt = env->priv;
  int err;

  bh = sb_bread(mnt->sb, BITTER_SUPER_OFFSET / BITTER_BLOCK_SIZE);
  if (!bh) {
		pr_err("write_super: cannot read block %d\n",
		       BITTER_SUPER_OFFSET / BITTER_BLOCK_SIZE);
		return -BITTER_EIO;
  }
  
  memcpy(bh->b_data, buf, BITTER_SUPER_SIZE);
  mark_buffer_dirty(bh);
  err = sync_dirty_buffer(bh);
  brelse(bh);  
  return err;

}

static const struct bitter_env_ops bitterfs_env_ops = {
	.read_block		= bitterfs_read_block,
	.alloc_block_buf	= bitterfs_alloc_block_buf,
	.dirty_block		= bitterfs_dirty_block,
	.put_block		= bitterfs_put_block,
	.flush_device		= bitterfs_flush_device,
	.writeback_all		= bitterfs_writeback_all,
	.read_super		= bitterfs_read_super,
	.write_super		= bitterfs_write_super,
};

/* See kernel/bitterfs.h for the ordering this owes its caller. */
void bitterfs_env_init(struct bitterfs_mount *mnt)
{
	mnt->env.ops  = &bitterfs_env_ops;
	mnt->env.priv = mnt;
}
