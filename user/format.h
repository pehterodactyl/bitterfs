#ifndef BITTER_USER_FORMAT_H
#define BITTER_USER_FORMAT_H

#include "format/bitterfs_format.h"

/* Each fills a whole BITTER_BLOCK_SIZE buffer, checksum included, ready to
 * write at the given address.  None can fail: every failure mode is an
 * invalid argument, which the caller is closer to.
 *
 * format_tree is the exception to "one output": besides the block, it
 * hands back the encoded root item its caller must insert into the ROOT tree.
 * It does not perform that insert -- it never sees the root tree's buffer. */

void format_super(void *buf, const bt_u8 *fsid,
  const char *label, bt_u64 total_bytes, bt_u64 root_bytenr,
  bt_u64 bytes_used);

void format_empty_leaf(void *buf, const bt_u8 *fsid, bt_u64 bytenr,
                                bt_u64 generation, bt_u64 owner);

/*
 * Produce a new tree: an empty leaf at `bytenr` owned by `objectid`, and the
 * bitter_root_item that names it, which the caller inserts into the root tree.
 *
 * Was format_extent_tree until phase 5.  The FS tree needed byte-for-byte the
 * same thing with a different owner, and phase 8 will need it again for every
 * subvolume -- so the objectid is a parameter rather than the difference
 * between two near-identical functions.
 */
void format_tree(void* buf, struct bitter_root_item *out, bt_u64 bytenr,
    bt_u64 generation, const bt_u8* fsid, bt_u64 objectid);

/*
 * Fill a freshly formatted FS tree leaf with a root directory containing one
 * empty regular file called `name`.
 *
 * Four items, in key order:
 *
 *   (256, INODE_ITEM,  0)            the root directory
 *   (256, DIR_ITEM,    hash(name))   the entry, as lookup finds it
 *   (256, DIR_INDEX,   2)            the same entry, as readdir walks it
 *   (257, INODE_ITEM,  0)            the file
 *   (257, EXTENT_DATA, 0)            where its bytes live
 *
 * `name` must be at most BITTER_MAX_FILENAME bytes and is stored without a
 * terminator.
 */
void format_initial_fs_tree(void* buf, const char* name, const char* content,
    bt_u64 data_bytenr);


#endif
