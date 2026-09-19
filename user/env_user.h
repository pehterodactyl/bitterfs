/*
 * The userspace side of the seam: core/ over pread and pwrite.
 *
 * struct env_user is defined here rather than in the .c because callers
 * declare one as a local -- an incomplete type could only be pointed at.
 * struct user_buf stays private, because env_user.c allocates those and hands
 * out struct bitter_buf * instead.
 */
#ifndef BITTER_ENV_USER_H
#define BITTER_ENV_USER_H

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"

/* What env->priv points at.  core/ never looks inside. */
struct env_user {
  int         fd;
  const char* path;      /* for error messages only */

  /* Sticky: put_block returns void and so cannot report a failed write, the
   * same bind brelse is in.  Record it here and let the caller check after a
   * run -- the pattern the kernel uses in mapping_set_error(), where fsync
   * reports what writeback could not.  Phase 3's writeback_all reads this. */
  int         io_error;

  /*
   * The third self-identifying check the format promises, and the last to be
   * implemented: header.fsid against the filesystem's own.  It catches what
   * csum and bytenr cannot -- a block left behind by a PREVIOUS mkfs at the
   * same address, which checksums perfectly and is exactly where it claims to
   * be, and simply belongs to a filesystem that no longer exists.
   *
   * Unknown until the superblock has been read, which happens after this
   * struct is wired up.  `have_fsid` gates the check so mkfs and the unit
   * fixtures -- which never read a superblock -- are unaffected.
   */
  bt_u8       fsid[BITTER_FSID_SIZE];
  int         have_fsid;
};

/*
 * Wires `env` to the userspace ops table and to `priv`.  The caller owns both
 * structs; nothing is allocated and there is nothing to free.
 *
 *     struct env_user   priv;
 *     struct bitter_env env;
 *     bitter_env_user_init(&env, &priv, fd, path);
 */
void bitter_env_user_init(struct bitter_env* env, struct env_user* priv,
                          int fd, const char* path);

/*
 * Arms the fsid check.  Separate from the init above because the fsid lives in
 * the superblock, and reading the superblock needs an environment -- so it
 * cannot be known when the environment is built.  bitter_fs_info_init calls
 * this as soon as it has one.
 *
 * Until it is called, read_block performs the other two checks and skips this
 * one.  Calling it twice with different values is a caller bug, not something
 * this guards against.
 */
void bitter_env_user_set_fsid(struct bitter_env* env, const bt_u8* fsid);

#endif
