#ifndef BITTER_FSCK_H
#define BITTER_FSCK_H

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/btree.h"

struct fsck_refs {
  bt_u64* bytenr;
  bt_size count;
  bt_size cap;

};

/*
 * Every block of one tree: a reference recorded per pointer, and each block
 * checked against what the pointer claimed.
 *
 * Returns the number of problems found, or a negative error.  `seen`
 * accumulates across calls -- one set for the whole filesystem, because a
 * refcount counts pointers from anywhere.
 */
long fsck_observe_tree(struct bitter_env* env, struct bitter_root* root,
        struct fsck_refs* seen);

#endif
