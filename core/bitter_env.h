/*
 * The seam — the only thing core/ knows about the world outside it.
 *
 * core/ holds the filesystem logic and cannot call pread, kmalloc or sb_bread:
 * it compiles into the kernel module AND into userspace tools and tests, so it
 * can include neither <unistd.h> nor <linux/buffer_head.h>.  Everything it
 * needs from the environment arrives through the function pointers below.
 *
 * That is what lets a B-tree bug be found by a userspace test in thirty
 * seconds instead of by a kernel panic and a reboot.
 *
 * NOT here: locking.  core/ is lock-free by contract and assumes its caller
 * has already serialised access — the kernel module holds one mutex around
 * every entry point.  See docs/skeleton.tex, "Locking".
 */
#ifndef BITTER_ENV

#define BITTER_ENV

#include "format/bitterfs_format.h"

/* Forward declared because the two structs below refer to each other: the ops
 * take a bitter_env *, and bitter_env holds a bitter_env_ops *. */
struct bitter_env;


/*
 * One block, as core/ sees it.  The kernel counterpart is struct buffer_head,
 * which has twenty fields; these are the only three core/ has any business
 * touching.
 *
 * No size field, unlike buffer_head: the kernel supports variable block sizes
 * and needs b_size to say which, while BITTER_BLOCK_SIZE is a compile-time
 * constant here.  A size field would carry one value forever and invite code
 * to read it instead of the constant.
 */
struct bitter_buf {

  /* Byte offset, NOT a block index.  buffer_head's b_blocknr counts blocks,
   * so env_kernel.c divides by BITTER_BLOCK_SIZE on the way through — one of
   * the conversions this seam exists to hide. */
  bt_u64  b_bytenr;
  
  /* BITTER_BLOCK_SIZE bytes.  A plain pointer rather than an accessor because
   * core/ touches it on every single item read. */
  void*   b_data;

  /* The environment's own handle, opaque to core/: a struct buffer_head * in
   * the kernel, whatever userspace finds convenient.  put_block needs it to
   * call brelse on the right thing. */
  void*   b_priv;

};

/*
 * What the environment must provide.  Four operations move blocks; the last two
 * are the commit protocol's, added at phase 3.  Memory allocation would join
 * them only if something outgrew the stack.
 *
 * Deliberately small: every member added is a member both implementations must
 * get right, and a signature guessed before its caller exists is a signature
 * that gets redesigned.
 */
struct bitter_env_ops {

  /* Fetch an existing block.  NULL on failure -- matching sb_bread, which is
   * what the kernel implementation wraps. */
  struct bitter_buf* (*read_block)(struct bitter_env* env, bt_u64 bytenr);
  
  /* A buffer for a block whose contents do not matter yet, because the caller
   * is about to overwrite all of it.  Distinct from read_block so a freshly
   * allocated block costs no read. */
  struct bitter_buf* (*alloc_block_buf)(struct bitter_env* env, bt_u64 bytenr);
  
  /* "I changed this."  The environment decides what that means -- writeback
   * scheduling in the kernel, a flag in userspace. */
  void (*dirty_block)(struct bitter_env* env, struct bitter_buf* buf);
 
  /* Release.  Every buffer from read_block or alloc_block_buf must reach this
   * exactly once, on every path including the error paths. */
  void (*put_block)(struct bitter_env* env,  struct bitter_buf* buf);


  /*
   * The superblock, which is the one block that cannot go through read_block
   * or dirty_block.  It checksums 512 bytes rather than the whole 4096, it is
   * a bitter_super rather than a bitter_header so every field sits at a
   * different offset, and the fsid read_block compares against is the value
   * THIS read supplies.
   *
   * `buf` is BITTER_SUPER_SIZE bytes in both directions, and the environment
   * neither validates nor checksums it: core/ owns what a valid superblock is,
   * and these move bytes.
   *
   * Added at phase 7 so that trans_commit could live in core/.  Everything
   * else in the commit protocol already went through this seam; the superblock
   * write was the single reason the ordering had to be duplicated per
   * environment -- in the one place where being wrong is invisible until a
   * power cut.
   *
   * 0, or a negative BITTER_* error.
   */
  int (*read_super)(struct bitter_env* env, void* buf);
  int (*write_super)(struct bitter_env* env, const void* buf);

  /*
   * Returns when everything already handed to the device is on stable storage
   * -- past any write cache, surviving power loss.  fsync in userspace; a
   * barrier or a FUA write in the kernel.
   *
   * The commit protocol calls this twice, and the two calls mean different
   * things.  The first makes the new tree's blocks durable, and until it has
   * returned the superblock must not be written at all.  The second makes the
   * superblock itself durable, and is the moment the transaction is over.
   *
   * 0, or a negative BITTER_* error.  A failure must abort the commit: the
   * alternative is a superblock naming blocks that may not be there.
   */
  int (*flush_device)(struct bitter_env* env);

  /*
   * Returns when every block dirtied since the last call has been handed to
   * the device.  Handed to, not durable -- that is flush_device's guarantee,
   * and the two are separate so a caller pays for the barrier once rather than
   * once per block.
   *
   * PRECONDITION: the caller has released every buffer.  An environment is
   * permitted to write a block on put_block rather than here -- env_user does,
   * and then has no list of outstanding buffers and so no way to write one
   * still held.  A block dirtied and not released is therefore not covered by
   * the guarantee above, in any environment, whether or not the one in front
   * of you happens to manage it.  For a commit this means every path released
   * first; the alternative is a superblock published over a block that never
   * reached the disk.
   *
   * The kernel implementation genuinely writes here.  The userspace one has
   * nothing left to write, because put_block pwrites on release; what it
   * returns is the accumulated verdict on writes that already happened, and it
   * is the only route by which a failed put_block can still reach a caller
   * (see io_error in user/env_user.h).
   *
   * 0, or a negative BITTER_* error.
   */
  int (*writeback_all)(struct bitter_env* env);
};

/*
 * The handle core/ threads through everything.  ops is the vtable; priv is the
 * environment's own state, which core/ never inspects -- an fd in userspace, a
 * struct super_block * in the kernel.
 *
 * Passed by pointer rather than being a global so the kernel module can serve
 * two mounted bitterfs filesystems at once.
 */
struct bitter_env {
  
  const struct bitter_env_ops *ops;

  void* priv;

};



#endif
