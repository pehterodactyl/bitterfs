/*
 * Keys, and the two block layouts built out of them.
 *
 * RECONSTRUCTED 2026-09-04 after the file was deleted.
 * Every declaration below is taken from the definition in core/items.c, so
 * the interface is exact.  The COMMENTS are rewritten and are not the
 * originals; treat them as a first draft rather than as settled reasoning.
 */
#ifndef BITTER_ITEMS_H
#define BITTER_ITEMS_H

#include "format/bitterfs_format.h"
#include "core/bitter_endian.h"

/*
 * A key in host order.
 *
 * Not packed, deliberately: it is never written anywhere, so the compiler may
 * align the fields and each read is one aligned load rather than the byte-wise
 * reassembly bt_get_le64 does.  (btrfs packs BOTH of its key types, which lets
 * it cast a disk key straight to a cpu key on little-endian hosts.  That is a
 * real optimisation, but it depends on the two layouts staying identical and
 * needs an assertion policing them.  We decode instead.)
 *
 * Field names match bitter_key exactly, so the conversion functions read as
 * three near-identical lines and a mismatch stands out.
 */
struct bitter_key_cpu {
  bt_u64 objectid;
  bt_u8  type;
  bt_u64 offset;
};

/* Ordering is (objectid, type, offset) -- the sort order the whole format is
 * built around.  Returns <0, 0, >0. */
int bitter_key_cmp(const struct bitter_key_cpu *a, const struct bitter_key_cpu *b);

/* Compares a key still on disk against one in host order, decoding as it goes.
 * The search hot path: avoids materialising a cpu key per candidate. */
int bitter_key_cmp_disk(const struct bitter_key *disk, const struct bitter_key_cpu *b);

/* For when a converted copy is genuinely needed -- reading a key_ptr in order
 * to descend, or writing a key into a newly inserted item.  Everything
 * multi-byte goes through the accessors; `type` is one byte and does not. */
void bitter_key_from_disk(struct bitter_key_cpu *dst, const struct bitter_key *src);
void bitter_key_to_disk(struct bitter_key *dst, const struct bitter_key_cpu *src);

/* Descriptor fields are little-endian on disk and unaligned inside a packed
 * struct, so even these two go through the accessors. */
static inline bt_u32 bitter_item_get_offset(const struct bitter_item* item) {
  return bt_get_le32(&item->offset);
}
static inline void bitter_item_set_offset(struct bitter_item* item, bt_u32 off) {
  bt_put_le32(&item->offset, off);
}
static inline bt_u32 bitter_item_get_size(const struct bitter_item* item) {
  return bt_get_le32(&item->size);
}
static inline void bitter_item_set_size(struct bitter_item* item, bt_u32 size) {
  bt_put_le32(&item->size, size);
}

/* Items in this leaf.  Reads header.nritems. */
bt_u32 bitter_leaf_nritems(const void* block);

/* Declared here so -Wmissing-prototypes checks the definition against it. */
void bitter_leaf_set_nritems(void* block, bt_u32 items);

/* The descriptor at `slot`.  The caller can read `size` and `offset` from it. */
struct bitter_item* bitter_leaf_item(void* block, bt_u32 slot);

/* The payload `slot`'s descriptor points at.  Untyped: the meaning is the
 * item's type, which only the caller knows. */
void* bitter_leaf_data(void *block, bt_u32 slot);

/*
 * Bytes available for one more item, descriptor included.
 *
 * The one that has to be exactly right, because insert consults it to decide
 * whether a split is needed -- an overestimate corrupts the leaf while an
 * underestimate splits early and merely wastes space.
 */
bt_u32 bitter_leaf_free_space(void* block);

/* Total bytes occupied by slots [from, to): descriptors and payloads both.
 * What split uses to decide how much moves. */
bt_u32 bitter_leaf_range_size(void* block, bt_u32 from, bt_u32 to);

/*
 * Inserts one item at `slot`, shifting everything at or after it.
 *
 *   1. memmove descriptors [slot, nritems) forward by one item's width.
 *   2. memmove the payloads belonging to those slots DOWN by `size`.
 *   3. Rewrite offset -= size for every descriptor now at slot+1..nritems.
 *      THE STEP THAT GETS SKIPPED: the descriptors moved in step 1, but their
 *      offset fields still point at where the payloads used to be.
 *   4. Write the new descriptor: key via bitter_key_to_disk, size, and offset
 *      = the previous slot-1 payload's offset minus size (or
 *      BITTER_LEAF_DATA_SIZE - size when inserting at slot 0).
 *   5. Copy the payload in.
 *   6. Bump nritems.
 *
 * Steps 1 and 2 must precede step 4, or they overwrite the descriptor just
 * written.  All four shifts overlap, so memmove and never memcpy.
 *
 * The caller must have checked bitter_leaf_free_space first; this cannot fail
 * and does not check.
 */
void bitter_leaf_insert(void* block, const struct bitter_key_cpu *key, bt_u32 slot,
     const void* data, bt_u32 size);

/* Removes the item at `slot`, closing the gap in both directions.  Asserts
 * slot < nritems: an out-of-range slot is the caller's bug, not the disk's. */
void bitter_leaf_remove(void* block, bt_u32 slot);

/*
 * Structural checks on a leaf, returned as a bitmask so one call reports every
 * way the block is wrong rather than only the first.  Zero means intact.
 */
#define BITTER_LEAF_ERR_NRITEMS  (1u << 0)  /* more items than could fit */
#define BITTER_LEAF_ERR_BOUNDS   (1u << 1)  /* a payload leaves the data area */
#define BITTER_LEAF_ERR_OVERLAP  (1u << 2)  /* two payloads share bytes */
#define BITTER_LEAF_ERR_ANCHOR   (1u << 3)  /* slot 0 not flush with the end */
#define BITTER_LEAF_ERR_GAP      (1u << 4)  /* free space is not contiguous */
#define BITTER_LEAF_ERR_ORDER    (1u << 5)  /* keys not strictly ascending */

bt_u32 bitter_leaf_check(void* block);

/*
 * The slot to split a full leaf at, chosen by BYTES rather than by count:
 * items vary in size, so an even item count can be a wildly uneven split.
 * Clamped to leave both halves non-empty.
 */
bt_u32 bitter_leaf_split_point(void* block);

/*
 * Internal nodes.
 *
 * nritems is the same header field in both, meaning "children" here rather
 * than "items", which is why bitter_leaf_nritems serves both.  A key_ptr is a
 * fixed 33 bytes with no separate payload, so entries are a plain array --
 * bitter_leaf_item with a different stride.
 */
struct bitter_key_ptr* bitter_node_key_ptr(void* block, bt_u32 slot);

/* The child's address and the generation copied from its header.  Read as a
 * pair during a descent: comparing them is what detects a stale pointer
 * without reading the child. */
bt_u64 bitter_node_blockptr(void* block, bt_u32 slot);
bt_u64 bitter_node_generation(void* block, bt_u32 slot);

/* How many more children fit: BITTER_MAX_CHILDREN - nritems. */
bt_u32 bitter_node_free_slots(void *block);

/* Inserts a child entry at `slot`.  One memmove of the entries and a bump of
 * nritems -- no payload area to shift, unlike a leaf. */
void bitter_node_insert(void* block, bt_u32 slot, const struct bitter_key_cpu *key,
    bt_u64 blockptr, bt_u64 generation);

#define BITTER_NODE_ERR_NRITEMS  (1u << 0)  /* more children than could fit */
#define BITTER_NODE_ERR_EMPTY    (1u << 1)  /* a node with no children */
#define BITTER_NODE_ERR_BLOCKPTR (1u << 2)  /* a child address that cannot be */
#define BITTER_NODE_ERR_ORDER    (1u << 3)  /* keys not strictly ascending */

bt_u32 bitter_node_check(void* block);

/*
 * Binary search over a fixed-stride array whose entries begin with a key.
 * Serves leaves and nodes both, which is why the stride is a parameter.
 *
 * Returns the slot where the key is, or where it would be inserted; `found`
 * distinguishes the two.  A descent needs the insertion point even on a miss,
 * so "not found" cannot be signalled through the return value.
 */
bt_u32 bitter_bsearch(const void* base, bt_u32 stride, bt_u32 n,
      const struct bitter_key_cpu* key, int* found);

#endif
