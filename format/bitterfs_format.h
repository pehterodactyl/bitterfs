
/*
 * bitterfs on-disk format.
 *
 * The single reference for the byte-level layout, included VERBATIM by all
 * four consumers: the kernel module, the freestanding core library, the
 * userspace tools, and the tests.  That is why it lives in its own directory
 * and contains only declarations -- no functions, no logic.
 *
 * TWO RULES:
 *
 *   1. This file includes NOTHING.  Not <stdint.h>: the kernel compiles with
 *      -nostdinc and 6.12 adds no -isystem to put the compiler's include
 *      directory back, so <stdint.h> genuinely does not exist for a module.
 *      Verified -- a module build fails with "stdint.h: No such file or
 *      directory".  The __UINT*_TYPE__ macros below need no header at all.
 *
 *   2. Every structure is packed and every size is asserted.  On-disk padding
 *      must be explicit and documented, never inserted by a compiler whose
 *      choices vary by target.
 */
#ifndef BITTERFS_FORMAT_H
#define BITTERFS_FORMAT_H

/*
 * Remove all compiler-inserted padding.  A GCC/Clang extension, not standard
 * C -- acceptable here because this is a Linux filesystem.
 *
 * Consequence to remember: member accesses become unaligned, and taking the
 * address of a packed member warns under -Waddress-of-packed-member.  Copy
 * members out rather than pointing into a structure.
 */
#define BITTER_PACKED __attribute__((__packed__))

/*
 * The integer vocabulary for the whole project, since core/ cannot reach
 * <stdint.h> either (it compiles into the module too).
 *
 * The "le" prefix is a PROMISE, not a mechanism: these are ordinary unsigned
 * integers and the compiler will not stop a host-order assignment.  Always go
 * through the accessors in core/.  A single byte has no byte order, which is
 * why bt_u8 has no "le" counterpart and why key.type and header.level use it.
 */
typedef __UINT8_TYPE__  bt_u8;
typedef __UINT16_TYPE__ bt_le16;
typedef __UINT32_TYPE__ bt_le32;
typedef __UINT64_TYPE__ bt_le64;

typedef __UINT16_TYPE__ bt_u16;
typedef __UINT32_TYPE__ bt_u32;
typedef __UINT64_TYPE__ bt_u64;

typedef __SIZE_TYPE__   bt_size;

/*
 * The macros above are the compiler's own answer for the target, so these
 * cannot fail on any platform GCC or Clang supports.  They are here for the
 * platform that surprises us: better a compile error than an image whose
 * fields are silently the wrong width.
 */
_Static_assert(sizeof(bt_u8)   == 1, "bt_u8 must be 1 byte");
_Static_assert(sizeof(bt_u16)  == 2, "bt_u16 must be 2 bytes");
_Static_assert(sizeof(bt_u32)  == 4, "bt_u32 must be 4 bytes");
_Static_assert(sizeof(bt_u64)  == 8, "bt_u64 must be 8 bytes");
_Static_assert(sizeof(bt_le16) == 2, "bt_le16 must be 2 bytes");
_Static_assert(sizeof(bt_le32) == 4, "bt_le32 must be 4 bytes");
_Static_assert(sizeof(bt_le64) == 8, "bt_le64 must be 8 bytes");



/* Fixed at compile time: sb_bread and the buffer cache work in units up to
 * PAGE_SIZE, so a larger node would need multi-page handling before anything
 * could mount at all. */
#define BITTER_BLOCK_SIZE   4096

/* Not offset 0, so a partition table or bootloader can coexist without either
 * party destroying the other. */
#define BITTER_SUPER_OFFSET 0x10000
/* 32 rather than 4 so the field's SIZE is decoupled from the algorithm: a
 * later csum_type can select sha256 without moving a single byte. */
#define BITTER_CSUM_SIZE    32

#define BITTER_CSUM_TYPE_CRC32   0
#define BITTER_CSUM_TYPE_XXHASH  1
#define BITTER_CSUM_TYPE_SHA256  2
#define BITTER_CSUM_TYPE_BLAKE2  3


/* A UUID.  128 bits gives uniqueness with no central registry, which matters
 * because filesystems get cloned, imaged and moved between machines. */
#define BITTER_FSID_SIZE    16

#define BITTER_MAGIC_SIZE   8
#define BITTER_MAGIC "Bi7hYni4"

/* Caps tree depth, and therefore sizes the path array that btree_search
 * carries from root to leaf. */
#define BITTER_MAX_LEVEL    8

/* 96 rather than the 77 the fields need: a multiple of 16 so every structure
 * lines up with hexdump rows, and it leaves the leaf data area at exactly
 * 4000 bytes, which makes capacity arithmetic doable in your head. */
#define BITTER_HEADER_SIZE  96
#define BITTER_HEADER_CONTENT_SIZE (BITTER_CSUM_SIZE + BITTER_FSID_SIZE \
                                    + 8 + 8 + 8 + 4 + 1)

#define BITTER_LABEL_SUPER_SIZE  32
#define BITTER_SUPER_FIELD_SIZE  160
#define BITTER_SUPER_SIZE        512

/* Relationships between the constants above, rather than their values. */
_Static_assert(BITTER_SUPER_OFFSET % BITTER_BLOCK_SIZE == 0,
               "the superblock must start on a block boundary");
_Static_assert(BITTER_SUPER_SIZE <= 512,
               "the superblock must fit one 512-byte sector: a torn write across "
               "two sectors would fail the checksum, and there is no second copy");
_Static_assert(BITTER_HEADER_SIZE % 16 == 0,
               "header size multiple of 16 keeps structures aligned to hexdump rows");
_Static_assert(BITTER_HEADER_CONTENT_SIZE <= BITTER_HEADER_SIZE,
               "header fields overflow the space allotted to them");

/*
 * Item types.
 *
 * The NUMBERS ARE NOT LABELS.  Keys sort (objectid, type, offset), so the
 * numeric order of these constants is the physical order items appear on disk
 * for a given object.  INODE_ITEM is 1 so an object's inode is always the
 * first item in its range; EXTENT_DATA is last of the file types because a
 * large file has thousands of them and that bulk belongs at the end -- which
 * also makes truncate a single contiguous range delete.
 *
 * The gaps are insertion room.  A later type (EXTENT_CSUM for data checksums,
 * XATTR_ITEM) must be able to land at the correct SORT POSITION without
 * renumbering anything that already exists on disk.
 */
#define BITTER_INODE_ITEM   1
#define BITTER_INODE_REF    25
#define BITTER_DIR_ITEM     48
#define BITTER_DIR_INDEX    72
#define BITTER_EXTENT_DATA  96
#define BITTER_ROOT_ITEM    120
#define BITTER_EXTENT_ITEM  144

/*
 * --- how a directory key's offset is computed -----------------------------
 *
 * PART OF THE ON-DISK FORMAT, not an implementation detail.  Every name in
 * every directory is keyed by this, so a second implementation must compute
 * bit-identical values, and changing it makes the files of every existing
 * filesystem unfindable by name.  Written here, rather than left implicit in
 * whatever code happens to compute it, for the same reason bitter_crc32c.h
 * pins its polynomial instead of trusting the table.
 *
 * A DIR_ITEM key is (dir_objectid, BITTER_DIR_ITEM, hash), where hash is:
 *
 *     CRC-32/ISCSI over the name bytes
 *     width=32  poly=0x1edc6f41  init=0xffffffff
 *     refin=true  refout=true    xorout=0xffffffff
 *     check=0xe3069283
 *
 *   - the NAME BYTES ONLY: no NUL, no length prefix, no directory id mixed in.
 *     `name_len` from the item bounds the input, and the name is not a C
 *     string on disk.
 *   - the 32-bit result is ZERO-EXTENDED to the key's 64-bit offset.  The top
 *     half is unused, deliberately: it is room, not an oversight.
 *   - the standard parameters, matching bt_block_csum, so a key offset can be
 *     verified by hand against any off-the-shelf crc32c when a lookup is
 *     misbehaving and the question is whether the key or the search is wrong.
 *
 * core/dir.c's bitter_name_hash is the single implementation; this paragraph
 * is the specification it answers to.
 *
 * --- the collision that follows ---------------------------------------------
 * 32 bits of hash means a directory reaches an even chance of SOME collision
 * at roughly 65,000 entries, which is not exotic.  Two names hashing alike
 * produce one key, and keys are unique within a tree -- so a directory entry
 * item must eventually be able to hold more than one entry, walked linearly
 * and distinguished by comparing full names.  bitter_dir_item is shaped to
 * allow that; nothing implements it yet.  See docs/LOG.md.
 *
 * A DIR_INDEX key is (dir_objectid, BITTER_DIR_INDEX, next_dir_index), which
 * involves no hashing at all -- see bitter_inode_item.
 */

/*
 * Reserved, never a real type.  Range scans need an upper bound that sorts
 * after every genuine item: to delete every item for object N you scan
 * (N, 0, 0) .. (N, BITTER_KEY_TYPE_MAX, (bt_le64)-1).  If 255 were assignable
 * the last item of that type would be missed and unlink would leak it.
 */
#define BITTER_KEY_TYPE_MAX 255

/*
 * The design intent of the numbers above, made machine-checkable.  Keys sort
 * (objectid, type, offset), so this chain IS the on-disk layout: an object's
 * inode item first, its bulky extent maps last.  A renumbering that breaks the
 * order would still compile and still work -- and would quietly stop truncate
 * being a contiguous range delete.  Fail the build instead.
 */
_Static_assert(BITTER_INODE_ITEM  < BITTER_INODE_REF,   "inode item must sort first");
_Static_assert(BITTER_INODE_REF   < BITTER_DIR_ITEM,    "backref before dirents");
_Static_assert(BITTER_DIR_ITEM    < BITTER_DIR_INDEX,   "hash dirent before index dirent");
_Static_assert(BITTER_DIR_INDEX   < BITTER_EXTENT_DATA, "dirents before file data map");
_Static_assert(BITTER_EXTENT_DATA < BITTER_ROOT_ITEM,   "fs-tree types grouped below root");
_Static_assert(BITTER_ROOT_ITEM   < BITTER_EXTENT_ITEM, "root item below extent-tree types");
_Static_assert(BITTER_EXTENT_ITEM < BITTER_KEY_TYPE_MAX,
               "255 is the range-scan sentinel and must exceed every real type");

/*
 * Reserved object ids.  Cannot be added to later without renumbering every
 * image ever made, which is why they are settled before mkfs exists.
 */

#define BITTER_INVALID_OBJECTID       0
#define BITTER_ROOT_TREE_OBJECTID     1
#define BITTER_EXTENT_TREE_OBJECTID   2
#define BITTER_FS_TREE_OBJECTID       4
#define BITTER_FIRST_FREE_OBJECTID    256
#define BITTER_LAST_FREE_OBJECTID     0xFFFFFFFFFFFFFF00ULL

/*
 * FIRST_FREE and LAST_FREE bracket the range assignable to files and
 * directories; both ends outside it are reserved.
 */
_Static_assert(BITTER_INVALID_OBJECTID     < BITTER_ROOT_TREE_OBJECTID,   "0 must stay unused");
_Static_assert(BITTER_ROOT_TREE_OBJECTID   < BITTER_EXTENT_TREE_OBJECTID, "tree ids ordered");
_Static_assert(BITTER_EXTENT_TREE_OBJECTID < BITTER_FS_TREE_OBJECTID,     "tree ids ordered");
_Static_assert(BITTER_FS_TREE_OBJECTID     < BITTER_FIRST_FREE_OBJECTID,  "tree ids below the free range");
_Static_assert(BITTER_FIRST_FREE_OBJECTID  < BITTER_LAST_FREE_OBJECTID,   "free range must be non-empty");




/*
 * The central abstraction.  Every item in every tree is addressed by one of
 * these, and the sort order -- objectid, then type, then offset -- is what
 * makes each important operation a contiguous range scan rather than a search
 * per element.
 *
 * 17 bytes, deliberately unaligned.  Packing costs a little access speed and
 * buys a layout that is identical on every target.
 */
struct bitter_key {
  bt_le64 objectid;  /* which object this item belongs to */
  bt_u8   type;      /* what kind of item -- and what `offset` below means */
  bt_le64 offset;    /* meaning is per-type: name hash, file offset, parent
                      * id, extent length, or unused.  One field, six jobs. */
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_key) == 17, "key size changed");
/* Offsets as well as size: a reordering that preserves the total would pass a
 * size check alone, and would silently change the on-disk layout. */
_Static_assert(__builtin_offsetof(struct bitter_key, objectid) == 0, "key layout");
_Static_assert(__builtin_offsetof(struct bitter_key, type)     == 8, "key layout");
_Static_assert(__builtin_offsetof(struct bitter_key, offset)   == 9, "key layout");

/*
 * Starts every tree block, internal node and leaf alike.
 *
 * Self-identifying by design: csum proves the contents are intact, fsid proves
 * the block belongs to THIS filesystem, and bytenr proves it was found where
 * it claims to live.  The last two catch what a checksum cannot -- a stale
 * block from a previous mkfs, or a misdirected write.  Both would checksum
 * perfectly, because both were once valid blocks of something else.
 */
struct bitter_header {
  bt_u8   csum[BITTER_CSUM_SIZE];  /* crc32c in bytes 0-3, rest zero; covers
                                    * everything AFTER this field */
  bt_u8   fsid[BITTER_FSID_SIZE];  /* must equal the superblock's */
  bt_le64 bytenr;                  /* this block's own address */
  bt_le64 generation;              /* the transaction that wrote it -- the CoW
                                    * witness.  Equal to the current
                                    * transaction means the block is already
                                    * ours and may be modified in place;
                                    * older means it must be copied first.
                                    * That comparison IS the CoW decision. */
  bt_le64 owner;                   /* which tree this block belongs to.  Lets
                                    * fsck confirm a block is reachable from
                                    * the tree that claims it, and is how a
                                    * block cross-linked between two trees is
                                    * detected -- a failure mode that
                                    * checksums and bytenr both miss, because
                                    * the block is intact and in the right
                                    * place, just owned by someone else. */
  bt_le32 nritems;                 /* items in a leaf, children in a node */
  bt_u8   level;                   /* 0 == leaf */
  bt_u8   reserved[BITTER_HEADER_SIZE - BITTER_HEADER_CONTENT_SIZE];
                                   /* zero on write: the checksum covers these
                                    * bytes, and a future version needs to
                                    * distinguish "never set" from a value */
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_header) == BITTER_HEADER_SIZE, "header size changed");
_Static_assert(__builtin_offsetof(struct bitter_header, csum)       ==  0, "header layout");
_Static_assert(__builtin_offsetof(struct bitter_header, fsid)       == 32, "header layout");
_Static_assert(__builtin_offsetof(struct bitter_header, bytenr)     == 48, "header layout");
_Static_assert(__builtin_offsetof(struct bitter_header, generation) == 56, "header layout");
_Static_assert(__builtin_offsetof(struct bitter_header, owner)      == 64, "header layout");
_Static_assert(__builtin_offsetof(struct bitter_header, nritems)    == 72, "header layout");
_Static_assert(__builtin_offsetof(struct bitter_header, level)      == 76, "header layout");

/*
 * An entry in an internal node: the key that begins a subtree, and where that
 * subtree's root block lives.  Generation is duplicated from the child's own
 * header so a stale pointer is detectable without reading the child.
 */
struct bitter_key_ptr {
  struct bitter_key key;        /* smallest key in the subtree below */
  bt_le64           blockptr;   /* address of the child block */
  bt_le64           generation; /* child's generation, cross-checked on read */
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_key_ptr) == 33, "node entry size changed");
_Static_assert(__builtin_offsetof(struct bitter_key_ptr, blockptr) == 17, "node entry layout");

/*
 * An entry in a leaf.  Leaves grow from both ends: these descriptors march
 * forward from the header while the payloads they point at stack backward
 * from the end of the block, with the free space as the gap between.
 */


struct bitter_item {
  struct bitter_key key;
  bt_le32           offset; /* payload start, measured from the END OF THE
                             * HEADER -- not from the start of the block */
  bt_le32           size;   /* payload length in bytes */
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_item) == 25, "leaf entry size changed");
_Static_assert(__builtin_offsetof(struct bitter_item, offset) == 17, "leaf entry layout");
_Static_assert(__builtin_offsetof(struct bitter_item, size)   == 21, "leaf entry layout");


/*
 * Where a tree's root block currently lives.
 *
 * Stored under (objectid, BITTER_ROOT_ITEM, 0) in the root tree, one per tree.
 * The key carries the objectid, which is why there is no such member here --
 * two copies could disagree.  The offset is 0 and means "unused"; if phase 6
 * gives it a meaning, 0 has to keep meaning the live root, because every image
 * already written has a 0 there.
 *
 * `level` is also in the root block's own header.  It is duplicated because a
 * descent needs the depth BEFORE the first read -- the same argument that put
 * root_level in the superblock.
 *
 * The root tree itself has no root item: the item would have to live inside
 * the tree it locates.  It comes from super.root instead.
 */
#define BITTER_ROOT_ITEM_SIZE 17

struct bitter_root_item {
  bt_le64 bytenr;
  bt_le64 generation;
  bt_u8   level;
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_root_item) == BITTER_ROOT_ITEM_SIZE,
               "root item size changed");
_Static_assert(__builtin_offsetof(struct bitter_root_item, bytenr)     ==  0, "root item layout");
_Static_assert(__builtin_offsetof(struct bitter_root_item, generation) ==  8, "root item layout");
_Static_assert(__builtin_offsetof(struct bitter_root_item, level)      == 16, "root item layout");


/*
 * One timestamp.  Split rather than a single count of nanoseconds because that
 * is the shape the kernel uses -- struct timespec64 -- so loading one is two
 * assignments and no arithmetic that could overflow or lose precision.
 *
 * bt_le64 seconds, not bt_le32: a 32-bit seconds field stops working in 2038,
 * and an on-disk format is exactly the wrong place to inherit that.  nsec fits
 * in 32 bits because it is bounded by 1,000,000,000.
 */
#define BITTER_TIMESPEC_SIZE 12

struct bitter_timespec {
  bt_le64 sec;
  bt_le32 nsec;
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_timespec) == BITTER_TIMESPEC_SIZE,
               "timespec size changed");

/*
 * One file or directory: everything about it that is not its contents.
 *
 * Stored under (objectid, BITTER_INODE_ITEM, 0), where the objectid IS the
 * inode number -- which is why no such field appears below.  The key carries
 * identity, the payload carries attributes, and storing the number twice would
 * create two values that can disagree.  INODE_ITEM is key type 1 so this sorts
 * first within an object's range, ahead of its dirents and its data map.
 *
 * The offset is unused and zero: an object has exactly one inode item, so
 * there is nothing to distinguish.
 *
 * --- no block pointers ----------------------------------------------------
 * A classic Unix inode IS the block map: direct pointers, then single, double
 * and triple indirect.  There is nothing like that here.  A file's contents
 * are described by separate (ino, EXTENT_DATA, file_offset) items in the same
 * tree, which is what makes a hole free (no item at all), truncate a
 * contiguous range delete, and an extent shareable between two files -- the
 * mechanism behind both reflink and snapshots.
 *
 * `size` and `nbytes` are therefore SUMMARIES of that map rather than the map:
 * size is the logical end of the file, nbytes is what its extents actually
 * occupy, and the difference between them is exactly the holes.
 *
 * --- fields whose validity depends on `mode` -------------------------------
 * `rdev` is meaningful only for S_ISCHR and S_ISBLK, and zero otherwise.
 * `next_dir_index` is meaningful only for directories.  Both look like
 * universal attributes and are not; read either without checking mode and it
 * will be zero at best.
 */
#define BITTER_INODE_ITEM_SIZE 112

struct bitter_inode_item {

  /* The transaction that last wrote this inode.  Nothing reads it yet; fsck
   * will, to catch an item newer than the tree that contains it. */
  bt_le64 generation;

  /* Logical end of the file.  For a directory, conventionally the size of its
   * dirent data rather than a byte count anything reads. */
  bt_le64 size;

  /* Bytes the file's extents actually occupy -- st_blocks, once scaled.
   * Differs from `size` exactly by the holes, which is why both exist.
   * Nothing maintains it until phase 7 has data extents to count. */
  bt_le64 nbytes;

  /* DIRECTORIES ONLY: the DIR_INDEX offset the next entry created here will
   * receive.  A high-water mark, never a count -- a directory that has had a
   * thousand files created and deleted holds 1002 and no entries.
   *
   * Monotonic because readdir's f_pos cookie IS this number: reusing a freed
   * value would let a resumed readdir miss an entry it never saw, or return
   * one it already had.  Starts at BITTER_DIR_START_INDEX, not 0, because
   * f_pos 0 and 1 are "." and "..", which readdir synthesises.
   *
   * bt_le64 because the value becomes bitter_key.offset, and a narrower field
   * would saturate while the keyspace still had room -- at which point the
   * only options are wrapping (which breaks the property above) or refusing
   * to create files in a directory that is not full. */
  bt_le64 next_dir_index;

  bt_le32 nlink;

  /* Two fields, not one bt_le64: uid and gid are independent values with
   * independent accessors (i_uid_write, i_gid_write), and two bt_le32 occupy
   * the same eight bytes anyway.  Combining would buy nothing and cost a
   * shift and a mask on every access. */
  bt_le32 uid;
  bt_le32 gid;

  /* File type and permission bits.  The type half is what tells the VFS this
   * is a directory, and what decides whether rdev and next_dir_index mean
   * anything at all. */
  bt_le32 mode;

  /* Device number for S_ISCHR and S_ISBLK, zero otherwise.
   *
   * Written through the kernel's huge_encode_dev(), never as a raw dev_t: that
   * helper's major/minor packing is the on-disk ABI, and dev_t's internal
   * representation is not.  bt_le64 for headroom -- Linux's dev_t is 32 bits
   * today -- and to match what btrfs stores.
   *
   * ext2 has no room for this and overlays it on its block-pointer array.
   * There are no block pointers here, so it gets a field of its own. */
  bt_le64 rdev;

  /* Per-inode flags: immutable, nodatacow, compression.  None defined yet.
   * Empty for the same reason the superblock's feature words are: the field
   * has to have existed since the first image for an unknown bit to be
   * detectable later. */
  bt_le64 flags;

  /* atime  contents were READ.  Never updated on a read-only mount, and the
   *        reason relatime and noatime exist -- otherwise reading writes.
   * ctime  inode METADATA changed: chmod, chown, link count, rename, and also
   *        whenever mtime does.  "Change", not "creation".  Userspace cannot
   *        set it -- utimensat reaches atime and mtime and not this -- and
   *        that unforgeability is why it is separate from mtime.
   * mtime  contents changed.  What ls -l shows and make compares.  On a
   *        directory, changes when an entry is added or removed.
   * otime  created.  Set once, never updated.  Not POSIX; statx exposes it as
   *        STATX_BTIME.  struct inode has nowhere to put it, so it stays
   *        unreadable until a bitterfs_inode wrapper exists. */
  struct bitter_timespec atime;
  struct bitter_timespec ctime;
  struct bitter_timespec mtime;
  struct bitter_timespec otime;

} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_inode_item) == BITTER_INODE_ITEM_SIZE,
               "inode item size changed");



/*
 * Offsets, so a field that silently moves fails the build rather than
 * misreading every image ever written.
 */
_Static_assert(__builtin_offsetof(struct bitter_inode_item, generation)     ==   0, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, size)           ==   8, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, nbytes)         ==  16, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, next_dir_index) ==  24, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, nlink)          ==  32, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, uid)            ==  36, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, gid)            ==  40, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, mode)           ==  44, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, rdev)           ==  48, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, flags)          ==  56, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, atime)          ==  64, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, ctime)          ==  76, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, mtime)          ==  88, "inode item layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_item, otime)          == 100, "inode item layout");




/*
 * The longest name a directory entry may hold.
 *
 * 255 because that is NAME_MAX, and the VFS rejects anything longer before a
 * filesystem method ever sees it -- so a larger value is unreachable and a
 * smaller one would refuse files that exist on every other filesystem.
 *
 * Defined here rather than used from <linux/limits.h> because core/ is
 * freestanding and cannot include it, and because mkfs, the kernel and fsck
 * must agree.  It is also the bound that makes a DISK-supplied name_len safe
 * to act on: without it, a corrupt length is a copy that runs off the end of
 * the leaf.
 */
/*
 * What a directory entry points at, as bitter_dir_item.type.
 *
 * A COMPACTION of the type half of the target's mode, cached in the entry so
 * readdir can fill d_type without reading a block per entry -- the difference
 * between ls being one tree walk and one walk plus a random read per file.
 *
 * Deliberately NOT the kernel's DT_* values, which these do not match.  DT_*
 * is a Linux userspace ABI number, and an on-disk format that stores it has
 * adopted somebody else's numbering permanently; kernel/dir.c converts at the
 * readdir boundary, as btrfs and ext2 both do.  (bitter_inode_item.mode is the
 * opposite case and stores POSIX verbatim: it carries permission bits too, and
 * there is no better encoding to invent.)
 *
 * UNKNOWN is 0 so that a zeroed entry is self-describing rather than claiming
 * to be a regular file.  readdir reporting DT_UNKNOWN is legal and merely
 * makes the caller stat the target.
 */
#define BITTER_FT_UNKNOWN   0
#define BITTER_FT_REG_FILE  1
#define BITTER_FT_DIR       2
#define BITTER_FT_CHRDEV    3
#define BITTER_FT_BLKDEV    4
#define BITTER_FT_FIFO      5
#define BITTER_FT_SOCK      6
#define BITTER_FT_SYMLINK   7
#define BITTER_FT_MAX       8

_Static_assert(BITTER_FT_MAX <= 0xFF,
               "the file type must fit bitter_dir_item.type");

#define BITTER_MAX_FILENAME 255

/*
 * HEADER size, not item size -- the only constant in this file that means
 * that, hence the name.  A directory entry's item is
 * BITTER_DIR_ITEM_HEADER_SIZE + name_len bytes, and the authoritative total is
 * bitter_item.size in the leaf.
 */
#define BITTER_DIR_ITEM_HEADER_SIZE 20

/*
 * One directory entry.
 *
 * Stored TWICE, under two keys, with identical payloads:
 *
 *     (dir_ino, BITTER_DIR_ITEM,  hash(name))   for lookup by name
 *     (dir_ino, BITTER_DIR_INDEX, seq)          for readdir
 *
 * Two orderings because two operations need opposite things.  Lookup has a
 * name and wants one search, so the key must sort by name.  readdir must be
 * resumable -- getdents hands userspace a cookie and continues later without
 * skipping or repeating -- so it needs a stable, monotonic ordering, which a
 * hash cannot give: hashes collide, and a new entry can land behind the
 * current position.  The cost is two items per entry, four touched by rename,
 * and a consistency check fsck does not have yet.
 *
 * --- location is a KEY, not an inode number -------------------------------
 * An ordinary entry names (ino, BITTER_INODE_ITEM, 0) in this tree.  An entry
 * for a SUBVOLUME names a root item in the ROOT tree instead.  Storing the
 * full key is what lets phase 8 add subvolumes without rewriting every
 * directory entry ever committed; 17 bytes buys that.
 *
 * --- type is deliberate duplication ---------------------------------------
 * The target inode already records its type in `mode`.  Caching it here lets
 * readdir fill d_type without reading a block per entry -- the difference
 * between ls being one tree walk and one walk plus a random read per file.
 *
 * --- the name ---------------------------------------------------------------
 * Trails the header, and is NOT a string: no NUL is stored, so `name_len` is
 * authoritative and comparisons are memcmp over name_len bytes, never strcmp.
 * sizeof(struct bitter_dir_item) is therefore the HEADER alone, and a reader
 * must check that bitter_item.size equals header + name_len -- either one
 * disagreeing is a corrupt item, not an assumption to make.
 *
 * --- UNDECIDED: hash collisions --------------------------------------------
 * Two different names can hash to one value, and keys are unique within a
 * tree, so both entries cannot exist as separate items.  btrfs packs several
 * entries into ONE item and walks them linearly, comparing full names.
 *
 * This struct does not foreclose that -- an item can hold a sequence of
 * header+name pairs and the layout is unchanged -- but nothing implements it,
 * so today a collision is unrepresentable.  Harmless while nothing creates
 * files.  See docs/LOG.md.
 */
struct bitter_dir_item {

  /* The key of what this entry points at -- NOT this item's own key. */
  struct bitter_key location;

  /* Bounded by BITTER_MAX_FILENAME.  bt_le16 rather than bt_u8: 255 fits in a
   * byte with nothing to spare, and a length field with no headroom is one
   * that cannot be validated against a larger bound later. */
  bt_le16 name_len;

  /* DT_REG, DT_DIR and friends -- the d_type values readdir reports. */
  bt_u8 type;

  /* Flexible array member: present so the layout is stated in the type rather
   * than in a comment, and so callers read di->name instead of (di + 1).
   * Excluded from sizeof, which is what makes the header constant above
   * correct. */
  bt_u8 name[];

} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_dir_item) == BITTER_DIR_ITEM_HEADER_SIZE,
               "dir item header size changed");

/*
 * Offsets, so a field that silently moves fails the build rather than
 * misreading every directory ever written.
 */
_Static_assert(__builtin_offsetof(struct bitter_dir_item, location) ==  0, "dir item layout");
_Static_assert(__builtin_offsetof(struct bitter_dir_item, name_len) == 17, "dir item layout");
_Static_assert(__builtin_offsetof(struct bitter_dir_item, type)     == 19, "dir item layout");

/* name_len must be able to express the longest legal name. */
_Static_assert(BITTER_MAX_FILENAME <= 0xFFFF,
               "name_len is 16 bits and must hold BITTER_MAX_FILENAME");




/*
 * f_pos 0 and 1 are "." and "..", which readdir synthesises rather than
 * storing, so a directory's first real entry takes 2.  Shared by mkfs, create
 * and readdir -- three places that must agree, which is why it is a constant
 * and not a literal in each.  btrfs calls the same value
 * BTRFS_DIR_START_INDEX.
 */
#define BITTER_DIR_START_INDEX 2



/*
 * How many references point at one extent.
 *
 * Stored under (bytenr, BITTER_EXTENT_ITEM, length): the extent's start
 * address is the objectid and its length is the offset, so the key carries the
 * identity entirely and the payload holds only the number that changes.  One
 * field is the whole struct for that reason, not for want of finishing it.
 *
 * An extent is free when it has NO item, not when it has an item holding
 * zero: the last reference deletes the item, so a stored 0 is corruption and
 * extent_dec_ref refuses it.  Wrong in one direction this leaks space; wrong
 * in the other it hands out a range that is still referenced, and that damage
 * stays silent until something overwrites it.
 *
 * Because the objectid is an address and keys sort (objectid, type, offset),
 * extent items sit in the tree in address order -- so the mount-time scan that
 * rebuilds the free-space map is one linear walk, and the gaps between items
 * are exactly the free space.
 *
 * `flags` describes the extent rather than counting it.  One bit is assigned;
 * the rest are room, and an unknown bit must be preserved rather than cleared,
 * so a newer version's extents survive an older tool.
 *
 * Still deliberately absent: `generation` (nothing reads it yet), and the
 * tree-block-versus-file-data distinction, which waits for phase 7 to have
 * data extents to distinguish.
 */
#define BITTER_EXTENT_ITEM_SIZE 16

/*
 * Allocated, and reachable from no tree: the reserved prefix and the
 * superblock, which mkfs records so the free-space scan does not hand them
 * out.  Without this bit fsck reports them as leaked on every filesystem that
 * has ever existed -- "nothing points at it" is exactly what a leak looks like
 * from the outside.
 */
#define BITTER_EXTENT_FLAG_RESERVED (1ULL << 0)

struct bitter_extent_item {
  bt_le64 refs;
  bt_le64 flags;
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_extent_item) == BITTER_EXTENT_ITEM_SIZE,
               "extent item size changed");
_Static_assert(__builtin_offsetof(struct bitter_extent_item, refs)  == 0,
               "extent item layout");
_Static_assert(__builtin_offsetof(struct bitter_extent_item, flags) == 8,
               "extent item layout");

/*
 * How a file extent stores its bytes.
 *
 * NONE is 0 and is never written, so a zeroed or half-written item is
 * detectably wrong rather than a claim.  That matters more here than for
 * BITTER_FT_*: an INLINE item says its data FOLLOWS the header, so a bogus
 * inline claim means reading arbitrary leaf bytes as file contents.
 *
 * Deliberately not btrfs's numbering, which puts INLINE at 0 for exactly the
 * layout this avoids.  Do not "fix" it to match.
 */
#define BITTER_FILE_EXTENT_NONE    0
#define BITTER_FILE_EXTENT_REG     1
#define BITTER_FILE_EXTENT_INLINE  2
#define BITTER_FILE_EXTENT_MAX     3

_Static_assert(BITTER_FILE_EXTENT_MAX <= 0xFF,
               "the extent type must fit bitter_extent_data.type");

/*
 * One run of a file's contents.
 *
 * Stored under (ino, BITTER_EXTENT_DATA, file_offset): the key says WHERE IN
 * THE FILE this run begins, and the payload says where its bytes are.  All of
 * one file's items are contiguous -- EXTENT_DATA is the highest fs-tree type,
 * so the bulky part of a large file sorts after its inode and its dirents,
 * which is also what makes truncate one range delete.
 *
 * --- there are no block pointers anywhere -----------------------------------
 * A classic Unix inode IS its block map: direct pointers, then single, double
 * and triple indirect.  bitter_inode_item has none of that.  A file's map is
 * these items, and the B-tree provides the indirection at whatever depth the
 * file's size demands -- so there is no special case to write and no limit to
 * grow out of.
 *
 * Three consequences fall out, and none is available to a pointer scheme:
 *
 *   - A HOLE is the ABSENCE of an item.  A sparse range costs nothing at all,
 *     and the correct read for it is zeroes rather than an error.
 *   - TRUNCATE is a contiguous range delete, not a walk of indirect blocks.
 *   - Two files can have items naming the SAME extent, which is reflink and is
 *     what makes a snapshot cheap.  Pointers inside an inode belong to that
 *     inode and nothing else can reference them.
 *
 * --- four numbers, three coordinate systems --------------------------------
 * The pairs describe different things and none is derivable from the others:
 *
 *   disk_bytenr + disk_num_bytes   the PHYSICAL extent.  This pair is the
 *                                  extent tree's key -- (disk_bytenr,
 *                                  EXTENT_ITEM, disk_num_bytes) -- so it must
 *                                  describe the WHOLE extent, never this
 *                                  file's part of it, or refcounting searches
 *                                  for a key that does not exist.
 *   offset + num_bytes             THIS FILE's window into that extent.
 *   the key's offset               where that window lands in the FILE.
 *
 * `offset` is the one that looks redundant and is not.  Overwrite 4K in the
 * middle of a 1M file: copy-on-write puts the new bytes elsewhere, and the
 * file then needs three items, the first and third both naming the original
 * extent at different offsets into it.  Without this field the only way to
 * express that is to physically split the extent -- a megabyte of I/O to
 * change four kilobytes, which defeats the point of CoW entirely.  It is the
 * same mechanism that makes reflink possible.
 *
 * `num_bytes` and `disk_num_bytes` differ whenever the stored bytes are not
 * the same length as the range they represent -- compression being the
 * obvious case, if it ever arrives.
 *
 * --- deliberately absent ----------------------------------------------------
 * compression, encryption and a generation.  btrfs also carries `ram_bytes`,
 * which exists only because splitting a COMPRESSED extent cannot predict the
 * resulting pieces' sizes; with no compression there is nothing to predict.
 */
#define BITTER_EXTENT_DATA_SIZE 33

struct bitter_extent_data {

  /* Where the bytes are.  Meaningless when type is INLINE, where the bytes
   * follow this header instead. */
  bt_le64 disk_bytenr;

  /* The size of the WHOLE extent on disk -- with disk_bytenr, the extent
   * tree's key.  Not this file's share of it. */
  bt_le64 disk_num_bytes;

  /* Where inside that extent this file's data starts.  Zero for an extent this
   * file wholly owns; non-zero after a partial overwrite or a reflink. */
  bt_le64 offset;

  /* How many bytes of the FILE this item covers, starting at the key's
   * offset.  Always the logical length, uncompressed. */
  bt_le64 num_bytes;

  /* BITTER_FILE_EXTENT_*.  Last so the four aligned numbers sit at 0, 8, 16
   * and 24, and so inline data -- if it ever exists -- begins immediately
   * after the header rather than in the middle of the struct. */
  bt_u8 type;

} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_extent_data) == BITTER_EXTENT_DATA_SIZE,
               "extent data size changed");

/*
 * Offsets, so a field that silently moves fails the build rather than
 * misreading every file on the device.
 */
_Static_assert(__builtin_offsetof(struct bitter_extent_data, disk_bytenr)    ==  0, "extent data layout");
_Static_assert(__builtin_offsetof(struct bitter_extent_data, disk_num_bytes) ==  8, "extent data layout");
_Static_assert(__builtin_offsetof(struct bitter_extent_data, offset)         == 16, "extent data layout");
_Static_assert(__builtin_offsetof(struct bitter_extent_data, num_bytes)      == 24, "extent data layout");
_Static_assert(__builtin_offsetof(struct bitter_extent_data, type)           == 32, "extent data layout");

/*
 * The inverse of a directory entry: given an inode, which directory names it
 * and under what name.
 *
 * Stored under (inode, BITTER_INODE_REF, parent_dir) -- and that key is the
 * whole idea.  A dirent is keyed by the DIRECTORY with the file in its payload;
 * this is keyed by the FILE with the directory in its offset.  The two point at
 * each other, which is what makes the namespace checkable: fsck can confirm
 * every dirent has a matching backref, and that an inode's nlink equals the
 * number of backrefs it carries.
 *
 * Type 25 puts it between INODE_ITEM (1) and DIR_ITEM (48), so an object's
 * backrefs sit immediately after its inode item and before the entries it
 * contains -- which for a directory means "who names me" is one seek from
 * "what do I name".
 *
 * --- what it is FOR ---------------------------------------------------------
 * Nothing reads it yet.  It exists for unlink, which has to decrement nlink and
 * cannot do that honestly without knowing how many names point at the inode:
 * the count on disk is exactly the number being repaired, so trusting it is
 * circular, and the alternative is scanning the whole FS tree for dirents.
 *
 * `index` is the second half of that.  Deleting a name means removing both the
 * DIR_ITEM and the DIR_INDEX, and the DIR_INDEX key is (dir, DIR_INDEX,
 * index) -- a value recorded nowhere else once the entry exists.  Without it
 * unlink would have to walk the directory comparing names to find the index it
 * is about to delete.
 *
 * link(2) adds a second of these; rename rewrites one.
 *
 * --- UNDECIDED: two links in one directory ----------------------------------
 * The key is unique per (inode, parent), so `ln a b` in a single directory
 * wants two backrefs under one key and cannot have them.  btrfs packs several
 * into one item and walks them comparing names.
 *
 * This is the SAME limitation bitter_dir_item has for name-hash collisions, and
 * it has the same shape of fix -- an item holding a sequence of header+name
 * pairs, with the layout below unchanged.  Nothing implements it.  Harmless
 * while nothing creates hard links.  See docs/LOG.md.
 */
#define BITTER_INODE_REF_SIZE 10

struct bitter_inode_ref {

  /* The DIR_INDEX this name was given when it was created -- the directory's
   * next_dir_index at that moment.  Recorded here because the dirent's own key
   * carries it and nothing else does, so an inode that wants to remove its own
   * name would otherwise have to search for it. */
  bt_le64 index;

  /* Bounded by BITTER_MAX_FILENAME.  bt_le16 for the same reason
   * bitter_dir_item uses one: 255 fits in a byte with nothing to spare, and a
   * length field with no headroom cannot be validated against a larger bound
   * later. */
  bt_le16 name_len;

  /* Flexible array member, excluded from sizeof -- which is what makes the
   * size constant above the HEADER size rather than the item size.  A reader
   * must check that bitter_item.size equals BITTER_INODE_REF_SIZE + name_len;
   * either disagreeing is a corrupt item, not an assumption to make. */
  bt_u8 name[];

} BITTER_PACKED;


_Static_assert(sizeof(struct bitter_inode_ref) == BITTER_INODE_REF_SIZE,
               "inode ref header size changed");

/* Offsets, so a field that silently moves fails the build rather than the
 * filesystem. */
_Static_assert(__builtin_offsetof(struct bitter_inode_ref, index)    ==  0,
               "inode ref layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_ref, name_len) ==  8,
               "inode ref layout");
_Static_assert(__builtin_offsetof(struct bitter_inode_ref, name)     == 10,
               "inode ref layout");

/*
 * The superblock -- the only structure at a fixed location, and the only one
 * never copy-on-written.  There is nowhere to put a pointer to a new copy, so
 * it is overwritten in place every commit.  That single overwrite IS the
 * transaction commit: everything else is CoW precisely so that this one write
 * can be the moment the transaction becomes real.
 *
 * Must fit in ONE 512-byte sector.  If it spanned two, a torn write could
 * leave sector 0 new and sector 1 old; the checksum would then fail and, with
 * a single copy, the filesystem would be unmountable.  Fields are ordered
 * largest-first, which incidentally lands every one on its natural alignment.
 */
struct bitter_super {

  bt_u8 csum[BITTER_CSUM_SIZE];    /* covers everything after this field */

  bt_u8 fsid[BITTER_FSID_SIZE];    /* the filesystem's UUID */

  bt_u8 magic[BITTER_MAGIC_SIZE];  /* identifies this as bitterfs at all */

  bt_le64 bytenr;                  /* its own address; catches a superblock
                                    * read from the wrong offset */

  bt_le64 generation;              /* the committed transaction number */

  bt_le64 root;                    /* address of the root tree's root block.
                                    * THE one pointer -- everything in the
                                    * filesystem is reached from here */

  bt_le64 total_bytes;             /* device size recorded at mkfs */

  bt_le64 bytes_used;              /* the bump allocator's high-water mark: an
                                    * ADDRESS, not a count.  Everything below
                                    * it is allocated, everything above is
                                    * free, and whoever opens the filesystem
                                    * restores root->next_free from it.
                                    *
                                    * Nothing else persists that mark, and
                                    * without it a reopened image hands out
                                    * addresses that already hold live blocks.
                                    *
                                    * It includes the reserved 64 KiB before
                                    * the superblock, so statfs derives a used
                                    * figure from it rather than reporting it.
                                    * The name is what it meant while nothing
                                    * read the field; with a bump allocator the
                                    * two quantities differ only by that
                                    * prefix, and at phase 4 freeing makes them
                                    * genuinely diverge.  See docs/LOG.md. */

  bt_le64 compat_flags;            /* unknown bit -> mount rw normally */

  bt_le64 ro_compat_flags;         /* unknown bit -> mount READ-ONLY: the
                                    * data is readable, but writing without
                                    * understanding the feature would corrupt
                                    * it.  Data checksums are the motivating
                                    * example */

  bt_le64 incompat_flags;          /* unknown bit -> refuse to mount */

  bt_le32 block_size;              /* 4096.  Stored although it is a compile
                                    * time constant, so a mismatched tool
                                    * DETECTS the difference instead of
                                    * misparsing every structure */

  bt_le16 csum_type;               /* which algorithm fills csum -- the reason
                                    * that field is 32 bytes */

  bt_u8 root_level;                /* depth of the root tree.  Redundant with
                                    * the root block's own header, and that is
                                    * the point: disagreement is detectable at
                                    * mount rather than mid-traversal */

  bt_u8 reserved0;                 /* one byte of alignment padding, so label
                                    * below starts at offset 128 and lands on
                                    * a hexdump row boundary.  Zero on write */

  bt_u8 label[BITTER_LABEL_SUPER_SIZE];  /* human-readable name for blkid */

  bt_u8 reserved[BITTER_SUPER_SIZE - BITTER_SUPER_FIELD_SIZE];
                                   /* future room in the one structure that
                                    * can never grow */
} BITTER_PACKED;

_Static_assert(sizeof(struct bitter_super) == BITTER_SUPER_SIZE, "superblock must fit one sector");
_Static_assert(__builtin_offsetof(struct bitter_super, csum)      ==   0, "super layout");
_Static_assert(__builtin_offsetof(struct bitter_super, magic)     ==  48, "super layout");
_Static_assert(__builtin_offsetof(struct bitter_super, root)      ==  72, "super layout");
_Static_assert(__builtin_offsetof(struct bitter_super, label)     == 128, "super layout");
_Static_assert(__builtin_offsetof(struct bitter_super, reserved)  == BITTER_SUPER_FIELD_SIZE,
               "BITTER_SUPER_FIELD_SIZE disagrees with the fields actually declared");

/* Derived, never hardcoded: change the header padding and this follows. */
#define BITTER_LEAF_DATA_SIZE (BITTER_BLOCK_SIZE - sizeof(struct bitter_header))

/* A leaf must hold enough entries for splitting to make progress; a tree whose
 * nodes fit one child cannot be balanced. */
_Static_assert(BITTER_LEAF_DATA_SIZE > 4 * sizeof(struct bitter_item),
               "leaf data area too small to be useful");
_Static_assert(BITTER_LEAF_DATA_SIZE > 4 * sizeof(struct bitter_key_ptr),
               "node data area too small to be useful");

/*
 * Capacity of one block, derived rather than stated: change the header padding
 * or a structure's fields and these follow.
 *
 * Today 160 and 121.  A node holds fewer than a leaf because a key_ptr (33) is
 * larger than an item descriptor (25) -- but an item also needs payload space,
 * so a leaf reaches its limit long before 160 in practice.  These are the
 * ceilings, not the expected occupancy.
 */
/* The largest payload that can ever be inserted: an otherwise empty leaf's
 * data area, less the descriptor the item itself needs.  Anything bigger fits
 * in no leaf however cleanly it is split, so btree_insert must reject it up
 * front rather than splitting forever. */
#define BITTER_MAX_ITEM_SIZE (BITTER_LEAF_DATA_SIZE - sizeof(struct bitter_item))

_Static_assert(BITTER_MAX_ITEM_SIZE == 3975, "maximum item size changed");

/*
 * The longest legal directory entry must fit in one item, or a create would
 * fail for a name the VFS considers perfectly valid -- and fail with no
 * visible cause, since nothing else would be wrong.
 *
 * Trivially true today (275 against 3975).  Asserted because the two bounds
 * move for entirely unrelated reasons: one tracks NAME_MAX, the other tracks
 * the block size.
 */
_Static_assert(BITTER_DIR_ITEM_HEADER_SIZE + BITTER_MAX_FILENAME
               <= BITTER_MAX_ITEM_SIZE,
               "a maximum-length directory entry must fit in one item");

/*
 * And an extent data header must leave room for something after it -- an
 * inline extent's bytes trail the header inside the same item.  Here rather
 * than beside the struct for the same reason as the bound above:
 * BITTER_MAX_ITEM_SIZE depends on sizeof(struct bitter_header) and so cannot
 * be defined until the layout section below.
 */
_Static_assert(BITTER_EXTENT_DATA_SIZE < BITTER_MAX_ITEM_SIZE,
               "an extent data header must fit in one item");

#define BITTER_MAX_ITEMS    (BITTER_LEAF_DATA_SIZE / sizeof(struct bitter_item))
#define BITTER_MAX_CHILDREN (BITTER_LEAF_DATA_SIZE / sizeof(struct bitter_key_ptr))

/*
 * A split must make progress: moving half the entries out of a full block has
 * to leave both halves smaller than the original.  With fewer than four, "half"
 * rounds badly and a split can produce a block that is still full -- an
 * infinite loop rather than a wrong answer, which is the harder kind to
 * diagnose.  The assertions above on the data area say the same thing in
 * bytes; these say it in entries, which is the unit split actually works in.
 */
_Static_assert(BITTER_MAX_ITEMS    >= 4, "leaf too small for splitting to make progress");
_Static_assert(BITTER_MAX_CHILDREN >= 4, "node too small for splitting to make progress");

/* Follows from sizeof(key_ptr) > sizeof(item).  Stated so that reordering or
 * growing either structure cannot silently invert the relationship the split
 * and merge thresholds are written against. */
_Static_assert(BITTER_MAX_CHILDREN < BITTER_MAX_ITEMS,
               "a node should hold fewer entries than a leaf");

/* A tree needs at least a root and a leaf, and bitter_path is sized by this. */
_Static_assert(BITTER_MAX_LEVEL >= 2, "a tree needs at least a root and a leaf");

/* Both are used as bt_u32 slot counts throughout core/, so they must fit. */
_Static_assert(BITTER_MAX_ITEMS    <= 0xFFFFFFFFu, "item count must fit bt_u32");
_Static_assert(BITTER_MAX_CHILDREN <= 0xFFFFFFFFu, "child count must fit bt_u32");

/*
 * ===========================================================================
 * Error codes
 * ===========================================================================
 * core/ returns NEGATIVE values of these, matching the kernel convention:
 * `return -BITTER_EIO;`, never a global errno.
 *
 * They exist because core/ cannot name EIO.  <errno.h> is not a freestanding
 * header and <linux/errno.h> exists only in a kernel build -- the same bind as
 * <string.h>.  The FUNCTIONS in that case were exported so a declaration
 * sufficed (see core/bitter_string.h), but these are macros, and a macro
 * cannot be declared.
 *
 * The VALUES deliberately match Linux's (include/uapi/asm-generic/errno*.h),
 * so the kernel module can `return ret;` straight to the VFS with no
 * translation.  A layer converting between two identical numbering schemes is
 * where off-by-one bugs live.
 *
 * Only the codes core/ actually returns are listed.  Add one when something
 * returns it, not before -- an unused error code is a promise about behaviour
 * that does not exist.
 */
#define BITTER_ENOENT   2
#define BITTER_EIO      5   /* checksum mismatch, short read, bad block */
#define BITTER_ENOMEM  12   /* the environment could not give us a buffer */
#define BITTER_EEXIST 17   /* the key is already in the tree */
#define BITTER_EINVAL  22   /* a caller passed something impossible */
#define BITTER_ENOSPC  28   /* no room: the device, or a block that will not fit */
#define BITTER_EROFS   30   /* write attempted on a read-only mount */
#define BITTER_EUCLEAN 117  /* on-disk structure failed its invariants */

/* The tree is corrupt rather than merely unreadable: a block that failed
 * bitter_leaf_check or bitter_node_check is EUCLEAN, "structure needs
 * cleaning", which is what btrfs returns for the same condition.  Reserve
 * EIO for cases where the bytes genuinely did not arrive. */

#endif
