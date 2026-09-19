/*
 * Block allocation.
 *
 * The one place in the filesystem that hands out an address.  Everything that
 * needs a block -- copy-on-write, both splits, a new root -- comes through
 * here, which is why replacing the bump allocator with the free-space map
 * changed this function and no signature above it.
 */
#ifndef BITTER_ALLOC_H
#define BITTER_ALLOC_H

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/btree.h"

/* Only ever a pointer here; core/trans.h is included by whoever dereferences
 * it.  Keeps this header from depending on the transaction layout. */
struct bitter_trans;

/*
 * A fresh block for `root` at `level`, handed back through `out` with its
 * header initialised -- including generation, stamped from `trans`, so a block
 * is born owned by the transaction that made it and is never CoW'd on its
 * first touch.
 *
 * Returns 0, or a negative error.  The two failures are distinguished here
 * rather than by the caller, which is the point of the out-parameter:
 *
 *   -BITTER_ENOSPC   the free-space map is empty
 *   -BITTER_ENOMEM   the environment would not give us a buffer
 *
 * `*out` is set to NULL on every error path.
 *
 * Takes the free space from root->fs_info, so the root must have been given
 * its back-pointer at mount.
 */
int bitter_alloc_block(struct bitter_env* env, struct bitter_root* root, 
      bt_u8 level, struct bitter_trans* trans, struct bitter_buf** out);


/*
 * A block for FILE DATA, handed back through `out` as a byte address.
 *
 * Separate from bitter_alloc_block for one reason, and it is the whole reason:
 * that function stamps a bitter_header into the block it returns.  Do that to a
 * data block and the first BITTER_HEADER_SIZE bytes of the user's file become
 * metadata.  A data block has no header, no checksum and nothing
 * self-identifying -- the only thing that can be verified about it is the
 * mapping that names it, which is why bitterfs_get_block bounds and aligns the
 * address it computes.
 *
 * Takes no env, which follows from the same fact: there is no buffer to fill
 * and nothing to write.  The block layer writes the folio later, through the
 * mapping.  All this function touches is the in-memory free map and the
 * transaction's delayed-ref set -- the same reason trans_add_delayed_ref takes
 * no env either.
 *
 * So `out` is a number, not a handle.  Nothing is held and nothing is owed:
 * contrast bitter_alloc_block, whose out-parameter is a buffer the caller must
 * eventually put_block.
 *
 * Returns 0, or a negative error, and the two are distinguished for the caller
 * because they need opposite responses:
 *
 *   -BITTER_ENOSPC   the free-space map is empty.  Ordinary: -ENOSPC to
 *                    userspace, the filesystem is fine.
 *   -BITTER_ENOMEM   the delayed-ref set is full.  NOT ordinary -- the caller
 *                    must abort the transaction.  See core/trans.h.
 *
 * `*out` is set to 0 on every error path, so a caller who tests the pointer
 * rather than the return still fails safe.  Address 0 is the reserved region,
 * so it is unambiguous.
 *
 * Takes the free space from root->fs_info, so the root must have been given its
 * back-pointer at mount.
 */
int bitter_alloc_data_block(struct bitter_root* root, struct bitter_trans* trans, bt_u64 *out);

#endif
