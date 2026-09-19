#include "user/trans.h"
#include "user/util.h"
#include "user/env_user.h"
#include "core/root.h"
#include "core/extent.h"

#include <string.h>
#include <stdio.h>


int bitter_fs_info_init(struct bitter_fs_info* fs, struct bitter_env* env,
            struct bitter_free_extent* free_arr, bt_u32 free_cap) {

  fs->env = env;

  /* Borrowed storage, empty until the scan fills it.  free_count starts at 0
   * deliberately: an allocation attempted before extent_build_free_map runs
   * gets -BITTER_ENOSPC rather than reading uninitialised entries. */
  fs->free       = free_arr;
  fs->free_cap   = free_cap;
  fs->free_count = 0;

  int r = bitter_read_super(env, &fs->tree_root);
  if (r < 0) {
    return r;
  }
  fs->tree_root.fs_info = fs;

  /*
   * Arm the fsid check now that the superblock has been read and validated.
   * Everything below -- bitter_find_root, the free-space scan, and every read
   * for the life of this mount -- gets it.  Deliberately after read_super:
   * reading the superblock is what supplies the value.
   */
  {
    struct bitter_super sb;
    int sr = bitter_read_super_raw(env, &sb);
    if (sr < 0) {
      return sr;
    }
    bitter_env_user_set_fsid(env, sb.fsid);
  }

  /* Device-wide, so it belongs here rather than in each root.  The copies in
   * bitter_root are the two fields core/btree.h:104 says phase 4 deletes. */
  fs->total_bytes = fs->tree_root.total_bytes;

  /* The root tree first, always: it comes from the superblock by fiat, and
   * every other tree is found through it. */
  r = bitter_find_root(env, &fs->tree_root, BITTER_EXTENT_TREE_OBJECTID,
                       &fs->extent_root);
  if (r < 0) {
    return r;
  }
  fs->extent_root.fs_info = fs;

  /* Last, and only once both back-pointers are set: the scan descends the
   * extent tree, and anything that reaches bitter_alloc_block dereferences
   * root->fs_info. */
  r = extent_build_free_map(fs);
  if (r < 0) {
    return r;
  }

  return 0;
}
