/*
 * core/ over pread and pwrite.  The counterpart is kernel/env_kernel.c over
 * sb_bread and brelse; core/ is written against neither.
 *
 * Deliberately dumb: no buffer cache, so two read_block calls for the same
 * bytenr return two independent buffers.  Phase 2 modifies blocks in place
 * with no CoW, where a stale cached copy would be a real hazard, and the point
 * of these tests is B-tree logic rather than caching.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "core/bitter_env.h"
#include "core/bitter_endian.h"
#include "env_user.h"
#include "util.h"
#include "core/bitter_crc32c.h"

/*
 * One allocation per block: the public part, the dirty flag, and the storage.
 *
 * `pub` MUST stay first.  put_block and dirty_block receive a
 * struct bitter_buf * and cast it straight back to this type, which is only
 * valid because C guarantees a struct's address equals its first member's.
 * Reorder these and the code still compiles, still runs, and writes the dirty
 * flag into b_bytenr.
 */
struct user_buf {
  struct bitter_buf pub;
  int             dirty;
  bt_u8     data[BITTER_BLOCK_SIZE];
};

static struct bitter_buf* user_env_read_block(struct bitter_env* env, bt_u64 bytenr);
static struct bitter_buf* user_env_alloc_block_buf(struct bitter_env* env, bt_u64 bytenr);
static void user_env_dirty_block(struct bitter_env* env, struct bitter_buf* buf);
static void user_env_put_block(struct bitter_env* env, struct bitter_buf* buf);
static int user_env_flush_device(struct bitter_env* env);
static int user_env_writeback_all(struct bitter_env* env);
static int user_env_read_super(struct bitter_env* env, void* buf);
static int user_env_write_super(struct bitter_env* env, const void* buf);
/*
 * One table for the whole program -- the function pointers never vary, only
 * priv does.  Designated initialisers so a forgotten member is NULL (a
 * predictable crash) rather than whatever was in memory.
 */
static const struct bitter_env_ops user_env_ops = {
  .read_block =        user_env_read_block,
  .alloc_block_buf =   user_env_alloc_block_buf,
  .dirty_block =       user_env_dirty_block,
  .put_block =         user_env_put_block,
  .flush_device =      user_env_flush_device,
  .writeback_all =     user_env_writeback_all,
  .read_super =        user_env_read_super,
  .write_super =       user_env_write_super,
};

void bitter_env_user_init(struct bitter_env* env, struct env_user *priv,
                  int fd, const char* path) {
  priv->fd = fd;
  priv->path = path;
  priv->io_error = 0;
  /* No fsid yet: the superblock has not been read, and reading it needs this
   * environment.  bitter_env_user_set_fsid arms the check afterwards. */
  priv->have_fsid = 0;
  memset(priv->fsid, 0, BITTER_FSID_SIZE);
  env->priv = priv;
  env->ops = &user_env_ops;
}

void bitter_env_user_set_fsid(struct bitter_env* env, const bt_u8* fsid) {
  struct env_user* user = (struct env_user*) env->priv;
  memcpy(user->fsid, fsid, BITTER_FSID_SIZE);
  user->have_fsid = 1;
}

/*
 * NULL on failure rather than die(), matching sb_bread.  core/ must have a
 * policy for an unreadable block, and this is what makes that policy run in a
 * userspace test rather than first in the kernel, where a mistake is a panic.
 */
static struct bitter_buf* user_env_read_block(struct bitter_env* env, bt_u64 bytenr) {
  struct env_user* user = (struct env_user*) env->priv;
  int fd = user->fd;
  const char* path = user->path;

  /* bytenr came from core/, computed from data on disk and possibly from a
   * corrupt pointer -- unlike env, which we constructed and can trust. */
  if (bytenr % BITTER_BLOCK_SIZE != 0) {
    fprintf(stderr, "read_block: %#" PRIx64 " is not block-aligned\n", bytenr);
    return NULL;
  }

  struct user_buf* buf = malloc(sizeof(struct user_buf));
  if (!buf) {
    fprintf(stderr, "read_block: out of memory\n");
    return NULL;
  }
  memset(buf, 0, sizeof(struct user_buf));

  ssize_t n = pread(fd, buf->data, BITTER_BLOCK_SIZE,(off_t)bytenr);
  /* Nothing has escaped yet, so plain free -- put_block is the CALLER's
   * obligation, and only from the return below. */
  if (n < 0) {
    fprintf(stderr, "read_block: %s at %#" PRIx64 ": %s\n",
            path, bytenr, strerror(errno));
    free(buf);
    return NULL;
  }
  if (n != BITTER_BLOCK_SIZE) {
    fprintf(stderr, "read_block: %s at %#" PRIx64 ": short read, %zd of %d\n",
            path, bytenr, n, BITTER_BLOCK_SIZE);
    free(buf);
    return NULL;
  }

  /*
   * The self-identifying checks.  Here rather than in core/ because the
   * environment is what moves the bytes, so it is where a block that did not
   * survive the trip should stop; core/ sees only NULL, which btree_search
   * turns into -BITTER_EIO.
   *
   * fsid is the third of these and is missing: there is no mounted superblock
   * to compare against yet.  See docs/LOG.md.
   */
  const struct bitter_header* hdr = (const struct bitter_header*) buf->data;
  bt_u32 want = bt_get_le32(hdr->csum);
  bt_u32 have = bt_block_csum(buf->data, BITTER_BLOCK_SIZE);
  if (want != have) {
    fprintf(stderr, "read_block: %s at %#" PRIx64 ": csum %08x, computed %08x\n",
            path, bytenr, want, have);
    free(buf);
    return NULL;
  }

  /* Catches a block read from the wrong offset -- a stale or corrupt pointer
   * in a parent -- which the checksum cannot, because the block is intact and
   * simply is not the one that was asked for. */
  if (bt_get_le64(&hdr->bytenr) != bytenr) {
    fprintf(stderr, "read_block: %s at %#" PRIx64 ": block claims %#" PRIx64 "\n",
            path, bytenr, bt_get_le64(&hdr->bytenr));
    free(buf);
    return NULL;
  }

  /* The third check, once the superblock has told us what to expect.  Ordered
   * after csum and bytenr deliberately: those two say the bytes are intact and
   * in the right place, and only then is "whose are they?" a meaningful
   * question. */
  if (user->have_fsid && memcmp(hdr->fsid, user->fsid, BITTER_FSID_SIZE) != 0) {
    fprintf(stderr, "read_block: %s at %#" PRIx64 ": belongs to another filesystem\n",
            path, bytenr);
    free(buf);
    return NULL;
  }

  /* In place, not into a copy: the caller receives a pointer INTO the
   * allocation, which is what makes the cast in put_block work. */
  buf->pub.b_bytenr = bytenr;
  buf->pub.b_data   = buf->data;
  buf->pub.b_priv   = NULL;   /* unused: the wrapper is recovered by cast */
  return &buf->pub;
}

/*
 * No read: the caller is about to overwrite the whole block.  Zeroed rather
 * than left as malloc garbage, because every byte is inside the checksum and
 * two runs must produce identical images.  The kernel counterpart is
 * sb_getblk, which likewise skips the I/O that sb_bread would do.
 */
static struct bitter_buf* user_env_alloc_block_buf(struct bitter_env* env, bt_u64 bytenr) {
  struct env_user* user = (struct env_user*) env->priv;

  struct user_buf* buf = malloc(sizeof(struct user_buf));
  if (!buf) {
    fprintf(stderr, "alloc_block_buf: out of memory\n");
    return NULL;
  }
  memset(buf, 0, sizeof(struct user_buf));

  /*
   * The environment stamps the fsid, not core/.
   *
   * Same division as the checksum: which filesystem a block belongs to is a
   * property of the environment that created it, and core/ has no way to know
   * it -- there is no fsid in bitter_root or bitter_trans, and putting one
   * there would be a third copy to keep in step.  bitter_alloc_block fills
   * every other header field; this is the one it cannot.
   *
   * Zero while the fsid is unknown, which is mkfs and the unit fixtures.  They
   * never arm the check, so they never fail it.
   */
  if (user->have_fsid) {
    memcpy(((struct bitter_header*) buf->data)->fsid, user->fsid,
           BITTER_FSID_SIZE);
  }

  buf->pub.b_bytenr = bytenr;
  buf->pub.b_data   = buf->data;
  buf->pub.b_priv   = NULL;
  return &buf->pub;
}

/* Only marks; the write happens in put_block.  env is unused here -- no state
 * is needed to set a flag. */
static void user_env_dirty_block(struct bitter_env* env, struct bitter_buf* buf) {
  struct user_buf* self = (struct user_buf*) buf;
  self->dirty = 1;
  (void)env;
}

/*
 * Flush if dirty, then free -- unconditionally, on every path.  Returns void,
 * matching brelse, so a failed write has no caller to report to: it goes to
 * stderr and into priv->io_error for someone to notice later.
 */
static void user_env_put_block(struct bitter_env* env, struct bitter_buf* buf) {
  struct user_buf* self = (struct user_buf*) buf;
  struct env_user* user = (struct env_user*) env->priv;

  if (self->dirty) {
    /* The only place a tree block is written, so the only place its checksum
     * needs computing.  core/ never maintains it: a block is modified many
     * times between the CoW that created it and the write that ends its life
     * in memory, and only the last of those states goes to disk. */
    bt_put_le32(((struct bitter_header*) buf->b_data)->csum,
                bt_block_csum(buf->b_data, BITTER_BLOCK_SIZE));

    ssize_t n = pwrite(user->fd, buf->b_data, BITTER_BLOCK_SIZE,
                       (off_t)buf->b_bytenr);
    if (n != BITTER_BLOCK_SIZE) {
      fprintf(stderr, "put_block: %s at %#" PRIx64 ": %s\n", user->path,
              buf->b_bytenr, n < 0 ? strerror(errno) : "short write");
      /* A short write is not a failed call, so errno is stale there. */
      if (!user->io_error) {
        user->io_error = (n < 0) ? errno : EIO;
      }
    }
  }

  /* Leaking on top of a lost write helps nobody. */
  free(self);
}

static int user_env_writeback_all(struct bitter_env* env) {
  struct env_user* user = (struct env_user*)env->priv;
  int io_error = user->io_error;
  if (io_error) {
    return -BITTER_EIO;
  }
  return 0;

}

static int user_env_flush_device(struct bitter_env* env) {
  struct env_user* user = (struct env_user*)env->priv;
  int fd = user->fd;
  int r = fsync(fd);
  if (r < 0) {
    fprintf(stderr, "flush_device %s: %s\n", user->path, strerror(errno));
    return -BITTER_EIO;
  } 
  return 0;
}

/*
 * The superblock, moved and nothing more.
 *
 * Added at phase 7 so trans_commit could live in core/: it was the only step
 * of a commit that did not already go through the seam.  core/ owns what a
 * valid superblock is -- magic, checksum, its own bytenr -- so these two
 * validate nothing and checksum nothing.
 *
 * pread and pwrite rather than read_block: the superblock sits at a fixed
 * offset, is checksummed over BITTER_SUPER_SIZE rather than the whole block,
 * and is a bitter_super rather than a bitter_header, so every self-identifying
 * check read_block makes would be made against the wrong bytes.
 */
static int user_env_read_super(struct bitter_env* env, void* buf) {

  struct env_user* user = env->priv;

  ssize_t n = pread(user->fd, buf, BITTER_SUPER_SIZE, (off_t)BITTER_SUPER_OFFSET);
  if (n < 0) {
    fprintf(stderr, "read_super: %s: %s\n", user->path, strerror(errno));
    return -BITTER_EIO;
  }
  if (n != BITTER_SUPER_SIZE) {
    fprintf(stderr, "read_super: %s: short read, %zd of %d\n",
            user->path, n, BITTER_SUPER_SIZE);
    return -BITTER_EIO;
  }
  return 0;
}

/*
 * THE write: the single overwrite that makes a transaction real.
 *
 * Deliberately does NOT flush.  Durability is trans_commit's second
 * flush_device, and keeping the two separate is what lets the commit protocol
 * state its own ordering rather than inheriting it from an environment.
 *
 * The sticky io_error is not consulted here.  A failure is returned
 * immediately, because unlike a dirtied block this write has a caller standing
 * over it who must abort rather than continue.
 */
static int user_env_write_super(struct bitter_env* env, const void* buf) {

  struct env_user* user = env->priv;

  ssize_t n = pwrite(user->fd, buf, BITTER_SUPER_SIZE, (off_t)BITTER_SUPER_OFFSET);
  if (n < 0) {
    fprintf(stderr, "write_super: %s: %s\n", user->path, strerror(errno));
    return -BITTER_EIO;
  }
  if (n != BITTER_SUPER_SIZE) {
    fprintf(stderr, "write_super: %s: short write, %zd of %d\n",
            user->path, n, BITTER_SUPER_SIZE);
    return -BITTER_EIO;
  }
  return 0;
}
