#ifndef BITTER_USER_TRANS_H
#define BITTER_USER_TRANS_H
#include "core/bitter_env.h"
#include "core/trans.h"
#include "core/btree.h"
#include "core/fs.h"   /* bitter_fs_info, bitter_free_extent: both named below */

/* bitter_read_super, trans_start and trans_commit moved to core/trans.h at
 * phase 7: with read_super and write_super in the seam, nothing about them is
 * userspace-specific, and the commit ordering should have one definition. */

/*
 * Turns an open environment into a mounted filesystem: reads the superblock,
 * finds the extent tree, wires each root's back-pointer, and builds the
 * free-space map.
 *
 * The one place the assembly order is known.  bitter_read_super and
 * bitter_find_root stay ignorant of bitter_fs_info -- they fill a plain root
 * and return -- so "did someone forget the back-pointer?" has a single answer
 * instead of being a question at every call site.
 *
 * `free_arr` is BORROWED, not owned: nothing here frees it, and it must
 * outlive the fs_info.  Its capacity cannot be recovered from the pointer,
 * which is why free_cap is passed separately.
 *
 * Stays in user/ for one reason only: it arms the fsid check by calling
 * bitter_env_user_set_fsid, which is env_user's.  Everything else it does is
 * now in core/ -- the kernel's equivalent is fill_super, which does the same
 * assembly with the same pieces.
 *
 * Returns 0, or a negative error from any of the three steps.  On failure
 * `fs` has been partly written -- unlike bitter_read_super, this cannot leave
 * the caller's handle untouched, because the extent tree is only findable
 * after tree_root is filled in.
 */
int bitter_fs_info_init(struct bitter_fs_info* fs, struct bitter_env* env,
        struct bitter_free_extent* free_arr, bt_u32 free_cap);

#endif
