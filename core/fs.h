/*
 * One mounted filesystem, in memory.
 *
 * Nothing here is ever written to disk -- no packing, no bt_le types, no
 * layout asserts.  The extent tree is the durable truth about what is
 * allocated; the free-space map below is a cache of what that tree already
 * says, discarded at unmount and rebuilt at the next one.
 *
 * At phase 5 the kernel module's sb->s_fs_info points at one of these, which
 * is where the name comes from.
 */
#ifndef BITTER_FS_H
#define BITTER_FS_H

#include "format/bitterfs_format.h"   /* bt_u32, bt_u64 */
#include "core/bitter_env.h"          /* struct bitter_env */
#include "core/btree.h"               /* struct bitter_root, held by value */

/* One free range.  The mount scan walks the extent tree, whose items sort by
 * address, and the gaps between consecutive items are exactly these -- already
 * in order, with no sorting step. */
struct bitter_free_extent {
  bt_u64 start;
  bt_u64 length;
};

struct bitter_fs_info {
  struct bitter_env *env;

  struct bitter_root  tree_root;
  struct bitter_root  extent_root;

  /*
   * The default subvolume's tree: inode items, directory entries, and the
   * file data map.  Found through the root tree at mount, like extent_root,
   * and held here because every inode lookup searches it -- re-finding it per
   * lookup would be a full tree descent for a value that cannot change while
   * the filesystem is mounted.
   *
   * One field rather than a set, because there is one subvolume.  Phase 8
   * makes that false: a snapshot is a second FS tree with its own root item,
   * and this becomes whatever holds several.
   */
  struct bitter_root  fs_root;

  bt_u64              total_bytes;

  /* core/ is freestanding and cannot allocate: this array is supplied by
   * whoever mounts the filesystem and is only ever used within free_cap. */
  struct bitter_free_extent *free;
  bt_u32              free_count;
  bt_u32              free_cap;
};

#endif
