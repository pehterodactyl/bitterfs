#ifndef BITTER_DIR_H
#define BITTER_DIR_H

#include "format/bitterfs_format.h"
#include "core/items.h"

bt_u64 bitter_name_hash(const void* name, bt_u32 len);

void bitter_dir_item_key(struct bitter_key_cpu *key, bt_u64 dir, bt_u64 hash);

void bitter_dir_index_key(struct bitter_key_cpu *key, bt_u64 dir, bt_u64 index);

/*
 * The backref's key: the file is the objectid, the directory is the offset --
 * the exact reverse of the two above, where the directory is the objectid.
 *
 * A builder matters more here than for the other two.  Both arguments are
 * inode numbers, so reversing them yields (dir, INODE_REF, ino): a well-formed
 * key for a different object, accepted silently because both are bt_u64.  The
 * dirent builders take a directory and a hash or an index, which are different
 * enough in kind that swapping them produces something obviously wrong.
 *
 * Used from both sides -- the create path writes the backref and the unlink
 * path deletes it -- so this is also where the two are kept from disagreeing
 * about which number goes where.
 */
void bitter_inode_ref_key(struct bitter_key_cpu *key, bt_u64 ino, bt_u64 dir);

#endif
