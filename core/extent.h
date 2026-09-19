/*
 * Reference counting on the extent tree.
 *
 * The extent tree is the filesystem's record of which ranges of the device are
 * in use and how many things point at each.  An extent exists in the tree if
 * and only if something references it: the last reference removes the item
 * rather than storing a zero.
 */
#ifndef BITTER_EXTENT_H
#define BITTER_EXTENT_H

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/btree.h"
#include "core/items.h"

struct bitter_trans;

/* Pointer-only here; core/fs.h has the definition.  Declared rather than
 * included because fs.h includes btree.h, which this header also needs. */
struct bitter_fs_info;

/* Rounds extent_apply_delayed_refs will make before deciding it is not
 * converging.  Generous: each round copy-on-writes fewer blocks than the last,
 * so a handful is the realistic maximum and anything approaching this is a
 * bug rather than a busy transaction. */
#define BITTER_MAX_DRAIN_ROUNDS 64

/*
 * The key an extent's item is stored under: (bytenr, BITTER_EXTENT_ITEM,
 * length).
 *
 * The start address is the objectid and the length is the offset, so the key
 * carries the extent's identity entirely -- which is why bitter_extent_item
 * holds only a refcount.
 *
 * One construction site, for the same reason as bitter_root_key: a lookup and
 * an insert that disagree about the key would look up an item that is present
 * and not find it.
 *
 * The objectid being an address also means extent items sit in the tree in
 * address order, so a scan of the whole tree walks the device from start to
 * end and the gaps between consecutive items are exactly the free space.
 */
void bitter_extent_key(struct bitter_key_cpu* key, bt_u64 bytenr, bt_u64 length);

/*
 * Records one more reference to the extent at `bytenr` of `length` bytes,
 * creating its item with a count of 1 if this is the first.
 *
 * A missing item is the NORMAL case here, not an error -- it is what a
 * freshly allocated block looks like.  That is the opposite of what the same
 * search result means in bitter_find_root or extent_dec_ref.
 *
 * Returns 0, or a negative error:
 *
 *   -BITTER_EUCLEAN  an item of the wrong length under an EXTENT_ITEM key
 *   -BITTER_ENOSPC   no room to create the item
 *   -BITTER_EIO      the tree could not be read
 *
 * Mutates fs_info->extent_root: the copy-on-write descent relocates the extent
 * tree's root, and that new address has to survive the call.
 */
int extent_inc_ref(struct bitter_fs_info* fs_info, bt_u64 bytenr, bt_u64 length,
        struct bitter_trans* trans);

/*
 * Drops one reference.  If it was the last, the item is DELETED rather than
 * written back with a count of zero -- absence is what means "free", and that
 * is what lets the free-space scan read the gaps between items.
 *
 * Returns 0, or a negative error:
 *
 *   -BITTER_ENOENT   no item for this extent.  Either a reference is being
 *                    released that was never taken, or the tree is damaged;
 *                    from here the two are indistinguishable, which is why
 *                    this returns rather than asserting.
 *   -BITTER_EUCLEAN  an item of the wrong length, or a stored count of zero,
 *                    which no correct path can produce.
 *
 * The count is tested before it is changed.  Decrementing a stored zero would
 * wrap to (bt_u64)-1 and leak that extent for the life of the filesystem,
 * since it could never count back down.
 *
 * Mutates fs_info->extent_root, as extent_inc_ref does.
 */
int extent_dec_ref(struct bitter_fs_info* fs_info, bt_u64 bytenr, bt_u64 length,
      struct bitter_trans* trans);


/*
 * Rebuilds fs->free by walking the whole extent tree.
 *
 * Extent items are keyed by address, so the walk visits every allocated range
 * in ascending order and the GAPS between them are the free space -- one
 * linear pass, no sorting, no second structure.  The map is a cache of what
 * the tree already says: discarded at unmount, rebuilt here, and never itself
 * written to disk.
 *
 * Resets fs->free_count, so it is idempotent and may be run again to rebuild.
 *
 * This is the boundary where disk data becomes an in-memory structure, so it
 * validates: an item of the wrong type, a zero length, a misaligned address,
 * or a range overlapping its predecessor is -BITTER_EUCLEAN.  That validation
 * is what earns the assertions bitter_alloc_block makes about the map
 * afterwards.
 *
 * Returns 0, or a negative error:
 *
 *   -BITTER_EUCLEAN  the extent tree is malformed
 *   -BITTER_ENOMEM   more free ranges than fs->free_cap can hold.  NOT ENOSPC:
 *                    the device may be nearly empty and the caller's array too
 *                    small, and a caller can sensibly retry with a bigger one.
 *   -BITTER_EIO      a block could not be read
 *
 * On failure the map is partly filled and describes only the low end of the
 * device.  It is the one output of this function that looks usable and is not.
 *
 * Reads only: ins_len 0, no transaction, no CoW.  It measures the tree and
 * structurally cannot change it.
 *
 * ASSUMES the extent tree describes the WHOLE device, reserved prefix and
 * superblock included -- otherwise the scan reports those as free and the
 * allocator hands out the filesystem's own metadata.  mkfs records them; see
 * docs/LOG.md, "The free map handed out the superblock".
 */
int extent_build_free_map(struct bitter_fs_info* fs);

/*
 * Drains the transaction's delayed-ref set into the extent tree, applying each
 * pending change with extent_inc_ref or extent_dec_ref.
 *
 * This is the other half of trans_add_delayed_ref, and the reason that
 * function exists: recording a reference at allocation time would modify the
 * extent tree, which copy-on-writes it, which allocates, which needs another
 * reference recorded.  Deferring breaks the cycle; this is where the debt is
 * paid, at a point where allocating is allowed.
 *
 * It is RE-ENTRANT BY DESIGN.  Applying an entry copy-on-writes the extent
 * tree, which allocates, which appends new entries to the set being drained --
 * so this loops until the set is empty rather than iterating a saved count.
 * Each round touches fewer blocks than the last, which is why it terminates.
 *
 * Must run BEFORE writeback_all, since it dirties extent-tree blocks, and
 * before bitter_update_root, since the extent tree's root moves while it runs.
 *
 * Returns 0, or a negative error from the underlying refcount calls.  A
 * failure partway leaves the set HALF APPLIED with no way to undo it: the
 * extent tree holds some of the transaction's changes and not others, so the
 * caller must abort rather than commit.
 */
int extent_apply_delayed_refs(struct bitter_fs_info* fs, struct bitter_trans* trans);

#endif
