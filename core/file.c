#include "core/file.h"

/*
 * (ino, BITTER_EXTENT_DATA, file_offset) -- one run of a file's contents.
 *
 * The offset is a position in the FILE, not on the device.  That is what makes
 * "which item covers byte N of this file" a single search, and it is also what
 * keeps the key STABLE under copy-on-write: an overwrite moves the bytes to a
 * new device address, but the logical position does not change, so only the
 * payload is rewritten and the item never has to be deleted and reinserted
 * somewhere else in the tree.
 *
 * Taking the type out of the caller's hands is the point.  A key's type is
 * part of its identity, and BITTER_EXTENT_DATA written by hand at a call site
 * is the one typo that would silently search a different item run and find
 * nothing -- the same reason bitter_dir_item_key and bitter_dir_index_key
 * exist rather than three assignments each.
 */
void bitter_file_extent_key(struct bitter_key_cpu *key, bt_u64 ino,
      bt_u64 file_offset) {
  key->objectid = ino;
  key->type     = BITTER_EXTENT_DATA;
  key->offset   = file_offset;
}
