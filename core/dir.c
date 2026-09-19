/*
 * Directory entries: the key arithmetic, and nothing that touches a tree.
 *
 * Companion to core/root.c and core/extent.c, which own the key construction
 * for their own item types.  Grouped by purpose rather than by mechanism --
 * the name hash borrows crc32c, but it is a directory concern and belongs
 * beside the keys it produces rather than beside the checksum code.
 */
#include "core/dir.h"

#include "core/bitter_crc32c.h"

/*
 * The offset half of a DIR_ITEM key.
 *
 * The algorithm is specified in format/bitterfs_format.h and is part of the
 * ON-DISK FORMAT: this function answers to that paragraph rather than defining
 * it.  Standard CRC-32/ISCSI parameters, so the value can be checked by hand
 * against any off-the-shelf crc32c when a lookup misbehaves and the question
 * is whether the key or the search is wrong.
 *
 * `name` is bytes, not a string: nothing on disk is NUL-terminated, so `len`
 * is the only thing that says where the name ends.  const void * rather than
 * const char * so that passing a terminated buffer and forgetting the length
 * is not a thing that reads naturally.
 *
 * The 32-bit result zero-extends into the key's 64-bit offset; the top half is
 * unused room.  bt_crc32c is a raw core -- it applies neither the initial
 * value nor the final XOR -- so both are applied here, exactly as
 * bt_block_csum does for blocks.
 */
bt_u64 bitter_name_hash(const void* name, bt_u32 len) {
  return (bt_u64)(bt_crc32c(BT_CRC32C_INIT, name, len) ^ BT_CRC32C_INIT);
}

/*
 * (dir, BITTER_DIR_ITEM, hash) -- the entry as lookup finds it.
 *
 * Takes a hash rather than a name so that a caller inserting an entry can
 * compute it once and use it for this key and for the name comparison that
 * follows a search, rather than hashing the same bytes twice.
 */
void bitter_dir_item_key(struct bitter_key_cpu *key, bt_u64 dir, bt_u64 hash) {
  key->objectid = dir;
  key->type     = BITTER_DIR_ITEM;
  key->offset   = hash;
}

/*
 * (dir, BITTER_DIR_INDEX, index) -- the same entry as readdir walks it.
 *
 * No hashing: the offset is the directory's own monotonic counter, taken from
 * next_dir_index in its inode item.  That number is also readdir's f_pos
 * cookie, which is why it must never be reused -- see bitter_inode_item.
 */
void bitter_dir_index_key(struct bitter_key_cpu *key, bt_u64 dir, bt_u64 index) {
  key->objectid = dir;
  key->type     = BITTER_DIR_INDEX;
  key->offset   = index;
}

void bitter_inode_ref_key(struct bitter_key_cpu *key, bt_u64 ino, bt_u64 dir) {
  key->objectid = ino;
  key->type     = BITTER_INODE_REF;
  key->offset   = dir;
}
