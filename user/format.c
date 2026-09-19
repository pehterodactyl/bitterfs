/*
 * Producing the blocks of a fresh filesystem.
 *
 * Split out of mkfs.c so that tests/ can build a real image -- a genuine
 * superblock with a genuine checksum -- through the same code the tool uses,
 * rather than through a copy of it.  A duplicated formatter is a definition
 * nobody compares; docs/LOG.md records what that cost the last time.
 */
#include "format.h"

#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "core/bitter_endian.h"
#include "user/util.h"
#include "core/items.h"
#include "core/root.h"
#include "core/dir.h"
#include "core/bitter_crc32c.h"

/*
 * Fills `buf` with a complete superblock block, ready to write at
 * BITTER_SUPER_OFFSET.
 *
 * Returns void because it cannot fail: every failure mode is an invalid input,
 * and main() catches those where it can name the offending argument.  `buf` is
 * void * because it writes the whole 4096-byte block, not just the 512-byte
 * struct at the front of it.
 */
void format_super(void *buf, const bt_u8 *fsid, 
  const char *label, bt_u64 total_bytes, bt_u64 root_bytenr,
  bt_u64 bytes_used) {

  /* Zero first: this is what makes reserved0, reserved[], the label padding,
   * csum bytes 4-31, the feature flags and the 3584-byte tail all correct.
   * Every one of them is inside the checksummed range. */
  memset(buf, 0, BITTER_BLOCK_SIZE);
  struct bitter_super* sb = (struct bitter_super*) buf;

  /* Its own address, so a superblock read from the wrong offset says so
   * instead of being believed. */
  bt_put_le64(&sb->bytenr, BITTER_SUPER_OFFSET);
  
  /* Byte array, so memcpy — a single byte has no order, and routing it through
   * an integer would reverse it on a big-endian host.  BITTER_MAGIC_SIZE (8),
   * not sizeof(BITTER_MAGIC) (9): the literal's NUL has no room and no use.
   *
   * This assertion would be better in format/bitterfs_format.h, where it would
   * guard the constant for every consumer rather than for this function. */
  _Static_assert(sizeof(BITTER_MAGIC) - 1 == BITTER_MAGIC_SIZE, 
                  "magic string length must match BITTER_MAGIC_SIZE");
  memcpy(sb->magic, BITTER_MAGIC, BITTER_MAGIC_SIZE);
    
  memcpy(sb->fsid, fsid, BITTER_FSID_SIZE); 
  
  /* 1, not 0.  generation is the CoW witness — from phase 3, "generation ==
   * current transaction" means a block may be modified in place.  Starting at
   * 0 would make every block mkfs wrote look modifiable to transaction 0, so
   * CoW silently would not happen. */
  bt_put_le64(&sb->generation, 1);
    
  /* THE pointer: everything is reachable from here, and replacing it
   * atomically is how a transaction commits. */
  bt_put_le64(&sb->root, root_bytenr);
  
  bt_put_le64(&sb->total_bytes, total_bytes);
  
  /* The allocator's high-water mark, passed in rather than derived.  It used
   * to be root_bytenr + one block, which was true only while the root was the
   * last thing written; the extent tree sits past it, and that derivation
   * would put the mark exactly on top of it.  Only the caller knows how many
   * blocks it laid down, so only the caller can compute this. */
  bt_put_le64(&sb->bytes_used, bytes_used);

  /*
   * compat_flags, ro_compat_flags and incompat_flags belong here (offsets
   * 96/104/112).  Not written: the memset already zeroed them, which is
   * correct — no features are defined yet.
   *
   * The fields still earn their place empty.  The mount check is
   * `flags & ~SUPPORTED` (see btrfs_check_features, fs/btrfs/disk-io.c:3079),
   * which only works if the fields have existed since the first image: an
   * unknown incompat bit refuses the mount, an unknown ro_compat bit forces
   * read-only, compat is ignorable.  Three bt_put_le64 calls go here when the
   * first feature bit exists.
   */

  /* put_le32, NOT put_le64 — block_size is bt_le32 at offset 120, with
   * csum_type, root_level and reserved0 directly behind it.  An 8-byte write
   * runs over all three, and nothing warns, since both accessors take void *.
   *
   * Stored despite being a compile-time constant so a tool built with a
   * different block size detects the mismatch instead of misparsing. */
  bt_put_le32(&sb->block_size, BITTER_BLOCK_SIZE);

  /* Written here AND in header.level at root_bytenr; disagreement is
   * detectable at mount rather than mid-traversal.  One of four cross-checks
   * between the two blocks mkfs writes — the others are bytenr, fsid and
   * generation.  Plain assignment: bt_u8 has no byte order. */
  sb->root_level = 0;

  bt_put_le16(&sb->csum_type, BITTER_CSUM_TYPE_CRC32);

  /* strlen with no -1: that belongs with sizeof of a literal, which counts the
   * NUL.  With the empty label — the default — strlen(label) - 1 wraps to
   * SIZE_MAX.  The memset already supplied the terminator and padding, and
   * main() has already checked the length. */
  memcpy(sb->label, label, strlen(label));

  /*
   * Last, because it covers everything above.  Bytes 32..512: a field cannot
   * be part of its own input, and the end is 512 rather than 4096 because that
   * is the sector the drive writes atomically — with one copy and no mirrors,
   * a torn write beyond it must not be able to fail the checksum.
   *
   * The ^ BT_CRC32C_INIT is the final XOR; bt_crc32c is a raw core and applies
   * neither it nor the initial value.
   */
  bt_put_le32(sb->csum, bt_block_csum(buf, BITTER_SUPER_SIZE));

}

/*
 * Fills `buf` with an empty tree block: a leaf with no items.
 *
 * Everything that varies per block is a parameter, so phases 4 and 5 call this
 * unchanged for the extent tree and the FS tree.  Only nritems and level are
 * fixed — "empty leaf" is what the name promises, and if either had to vary
 * this would be format_block instead.
 */
void format_empty_leaf(void *buf, const bt_u8 *fsid, bt_u64 bytenr,
                                bt_u64 generation, bt_u64 owner) {
  
  memset(buf, 0, BITTER_BLOCK_SIZE);
  struct bitter_header* header = (struct bitter_header*) buf;

  /* Must equal the superblock's; a block whose fsid differs is a stale block
   * from a previous mkfs, which would checksum perfectly. */
  memcpy(header->fsid, fsid, BITTER_FSID_SIZE);  

  /* Its own address, so a misdirected write is detectable — also intact, also
   * correctly checksummed, just in the wrong place. */
  bt_put_le64(&header->bytenr, bytenr);
  
  bt_put_le64(&header->generation, generation);

  /* Which tree owns this block.  Lets fsck catch a block cross-linked into two
   * trees, which csum and bytenr both miss. */
  bt_put_le64(&header->owner, owner);

  /* Both already zero from the memset; written so the field list here matches
   * the struct's.  nritems is bt_le32 and needs the accessor even for 0 —
   * phase 2 copies this line with a real count. */
  bt_put_le32(&header->nritems, 0);
  
  header->level = 0;

  /* BITTER_BLOCK_SIZE, not BITTER_SUPER_SIZE: a tree block's items fill the
   * whole 4096, so a shorter range would leave them unprotected.  The
   * superblock's 512 limit came from sector atomicity, which does not apply to
   * a block that is not the commit point. */
  bt_put_le32(header->csum, bt_block_csum(buf, BITTER_BLOCK_SIZE));
}


/*
 * A new tree, in the two pieces every tree needs: the block, and the root item
 * that names it.
 *
 * `objectid` is what makes this general.  It is stamped into the leaf's
 * header.owner AND into the bitter_root, so a block cross-linked into two
 * trees is detectable by fsck -- a failure csum, bytenr and fsid all miss,
 * because the block is intact, in the right place, and belongs to somebody
 * else.
 *
 * The caller still owns two things this cannot do: inserting `out` into the
 * root tree under bitter_root_key(objectid), and recording the block in the
 * extent tree.  Both need buffers this function has never seen.
 */
void format_tree(void* buf, struct bitter_root_item *out, bt_u64 bytenr,
      bt_u64 generation, const bt_u8* fsid, bt_u64 objectid) {

  format_empty_leaf(buf, fsid, bytenr, generation, objectid);

  struct bitter_root root;
  memset(&root, 0, sizeof(struct bitter_root));

  /* level 0: a fresh tree is one leaf, which is also its root. */
  root.level = 0;
  root.bytenr = bytenr;
  root.objectid = objectid;
  root.generation = generation;

  root_item_to_disk(out, &root);
}

/* The root directory, and the first file in it.  Both are assigned from
 * BITTER_FIRST_FREE_OBJECTID upward: everything below it is reserved for
 * trees, which is why the root directory is 256 and not 1. */
#define ROOT_DIR_INO  (BITTER_FIRST_FREE_OBJECTID)
#define FIRST_FILE_INO (BITTER_FIRST_FREE_OBJECTID + 1)

/*
 * Build one inode item payload.
 *
 * `next_dir_index` is meaningful only for directories and zero otherwise, and
 * `size` means different things for the two: the logical end of a file, and by
 * convention the sum of its entries' name lengths for a directory.  Nothing
 * reads a directory's size -- readdir does not -- but stat shows it, and a
 * directory reporting 0 while holding files reads as broken.
 */
static void build_inode(struct bitter_inode_item* out, bt_u32 mode,
                        bt_u32 nlink, bt_u64 size, bt_u64 next_dir_index,
                        const struct timespec* now) {

  memset(out, 0, sizeof(struct bitter_inode_item));

  bt_put_le32(&out->mode, mode);
  bt_put_le32(&out->nlink, nlink);

  /* Owned by root: mkfs has no better answer, and the mount is read-only. */
  bt_put_le32(&out->uid, 0);
  bt_put_le32(&out->gid, 0);

  bt_put_le64(&out->size, size);

  /* No extents yet -- the file is empty and a directory has none by
   * construction.  nbytes counts what the data map occupies, and there is no
   * data map until phase 6. */
  bt_put_le64(&out->nbytes, 0);

  bt_put_le64(&out->next_dir_index, next_dir_index);

  /* 1, matching every other block mkfs writes: generation is the CoW witness,
   * and starting at 0 would make these look modifiable in place to
   * transaction 0.  See format_super. */
  bt_put_le64(&out->generation, 1);

  /* All four the same instant: a filesystem that has just been created has
   * been accessed, changed, modified and born at the same moment. */
  {
    struct bitter_timespec *ts[] = {
      &out->atime, &out->ctime, &out->mtime, &out->otime
    };
    unsigned i;
    for (i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
      bt_put_le64(&ts[i]->sec, (bt_u64) now->tv_sec);
      bt_put_le32(&ts[i]->nsec, (bt_u32) now->tv_nsec);
    }
  }

  /* rdev stays zero: meaningful only for S_ISCHR and S_ISBLK.  flags likewise:
   * no bits are defined. */
}

/*
 * A root directory holding one empty file.
 *
 * Four items, and their SLOTS are the key ordering written out by hand --
 * bitter_leaf_insert takes the slot it is given and does not re-sort.  Keys
 * sort (objectid, type, offset), so everything under 256 precedes 257, and
 * within 256 the inode (type 1) precedes the hash entry (48) precedes the
 * index entry (72).
 *
 * The two dirent items carry IDENTICAL payloads and differ only in their keys.
 * That is the whole design: lookup needs name ordering, readdir needs a stable
 * resumable cookie, and no single ordering provides both.
 */
void format_initial_fs_tree(void* buf, const char* name, const char* content,
      bt_u64 data_bytenr) {

  struct bitter_inode_item inode;
  struct bitter_key_cpu    key;
  struct timespec          now;
  size_t                   name_len = strlen(name);
  size_t                   content_len = strlen(content);

  /* The caller controls `name`, and mkfs is the only caller -- so a name too
   * long to store is a bug here rather than bad input. */
  if (name_len == 0 || name_len > BITTER_MAX_FILENAME) {
    die("format_initial_fs_tree: name must be 1..%d bytes, got %zu",
        BITTER_MAX_FILENAME, name_len);
  }

  /* CLOCK_REALTIME rather than time(NULL): the format stores nanoseconds, and
   * fill_super claims s_time_gran = 1.  A zero nsec would be a lie about
   * precision the superblock already advertises. */
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    now.tv_sec = 0;
    now.tv_nsec = 0;
  }

  /* --- slot 0: the root directory ------------------------------------- */
  /* nlink 2, not 1: a directory's link count is one per name pointing at it
   * plus one for its own ".".  The root is its own parent, so the two are its
   * entry in itself and its own dot.  Adding a FILE does not change this --
   * only a subdirectory would, through its "..". */
  build_inode(&inode, S_IFDIR | 0755, 2, (bt_u64) name_len,
              BITTER_DIR_START_INDEX + 1, &now);

  memset(&key, 0, sizeof key);
  key.objectid = ROOT_DIR_INO;
  key.type     = BITTER_INODE_ITEM;
  key.offset   = 0;
  bitter_leaf_insert(buf, &key, 0, &inode, sizeof inode);

  /* --- slot 1: the root's backref ------------------------------------- */
  {
    /*
     * The root directory is its own parent, so its backref names itself and
     * carries "..", which is the name that relationship is spelled with.
     * btrfs does the same.
     *
     * index 0, not BITTER_DIR_START_INDEX: ".." is synthesised by readdir at
     * f_pos 1 and has no DIR_INDEX item, so there is no index to record.  A
     * nonzero value here would name an entry that does not exist.
     */
    bt_u8 ref[BITTER_INODE_REF_SIZE + BITTER_MAX_FILENAME];
    struct bitter_inode_ref* ir = (struct bitter_inode_ref*) ref;

    memset(ref, 0, sizeof ref);
    bt_put_le64(&ir->index, 0);
    bt_put_le16(&ir->name_len, 2);
    memcpy(ir->name, "..", 2);

    /* objectid is the FILE and offset is the DIRECTORY -- the reverse of a
     * dirent's key, and here they are the same number. */
    memset(&key, 0, sizeof key);
    key.objectid = ROOT_DIR_INO;
    key.type     = BITTER_INODE_REF;
    key.offset   = ROOT_DIR_INO;
    bitter_leaf_insert(buf, &key, 1, ref, BITTER_INODE_REF_SIZE + 2);
  }

  /* --- slots 2 and 3: the entry, twice -------------------------------- */
  {
    /* The payload is variable length: a fixed header with the name directly
     * behind it, so it has to be assembled contiguously before the insert.
     * bitter_leaf_insert takes one pointer and one size. */
    bt_u8 entry[BITTER_DIR_ITEM_HEADER_SIZE + BITTER_MAX_FILENAME];
    struct bitter_dir_item* di = (struct bitter_dir_item*) entry;
    bt_u32 entry_size = (bt_u32) (BITTER_DIR_ITEM_HEADER_SIZE + name_len);
    struct bitter_key_cpu target;

    memset(entry, 0, sizeof entry);

    /* location is a KEY, not an inode number.  For a subvolume entry it would
     * name a root item in the ROOT tree instead, which is what makes phase 8
     * possible without rewriting every directory entry. */
    memset(&target, 0, sizeof target);
    target.objectid = FIRST_FILE_INO;
    target.type     = BITTER_INODE_ITEM;
    target.offset   = 0;
    bitter_key_to_disk(&di->location, &target);

    bt_put_le16(&di->name_len, (bt_u16) name_len);
    di->type = BITTER_FT_REG_FILE;

    /* memcpy, not strcpy: no NUL is stored, and name_len is what says where
     * the name ends. */
    memcpy(di->name, name, name_len);

    bitter_dir_item_key(&key, ROOT_DIR_INO, bitter_name_hash(name, (bt_u32) name_len));
    bitter_leaf_insert(buf, &key, 2, entry, entry_size);

    /* Same bytes, different key.  BITTER_DIR_START_INDEX because this is the
     * directory's first entry and f_pos 0 and 1 belong to "." and "..". */
    bitter_dir_index_key(&key, ROOT_DIR_INO, BITTER_DIR_START_INDEX);
    bitter_leaf_insert(buf, &key, 3, entry, entry_size);
  }

  /* --- slot 4: the file ------------------------------------------------ */
  /*
   * nlink 1: one name points at it.
   *
   * size is the LOGICAL length -- what read() returns -- while nbytes is what
   * the extent occupies, a whole block.  They differ because a short file
   * still costs the allocation granularity, and it is nbytes that stat
   * reports as st_blocks.
   */
  build_inode(&inode, S_IFREG | 0644, 1, (bt_u64) content_len, 0, &now);
  bt_put_le64(&inode.nbytes, BITTER_BLOCK_SIZE);

  memset(&key, 0, sizeof key);
  key.objectid = FIRST_FILE_INO;
  key.type     = BITTER_INODE_ITEM;
  key.offset   = 0;
  bitter_leaf_insert(buf, &key, 4, &inode, sizeof inode);

  /* --- slot 5: the file's backref -------------------------------------- */
  {
    /*
     * The other half of the entry written above: that one says the directory
     * holds this name, this one says the file answers to it.  nlink 1 on the
     * inode is the count of exactly these.
     *
     * index matches the DIR_INDEX the entry was given, which is what lets
     * unlink find and delete that entry without searching the directory.
     */
    bt_u8 ref[BITTER_INODE_REF_SIZE + BITTER_MAX_FILENAME];
    struct bitter_inode_ref* ir = (struct bitter_inode_ref*) ref;

    memset(ref, 0, sizeof ref);
    bt_put_le64(&ir->index, BITTER_DIR_START_INDEX);
    bt_put_le16(&ir->name_len, (bt_u16) name_len);
    memcpy(ir->name, name, name_len);

    memset(&key, 0, sizeof key);
    key.objectid = FIRST_FILE_INO;
    key.type     = BITTER_INODE_REF;
    key.offset   = ROOT_DIR_INO;
    bitter_leaf_insert(buf, &key, 5, ref,
                       (bt_u32)(BITTER_INODE_REF_SIZE + name_len));
  }

  /* --- slot 6: where the file's bytes are ------------------------------- */
  {
    struct bitter_extent_data ed;
    memset(&ed, 0, sizeof ed);

    /* The PHYSICAL extent: a whole block, because that is the allocation
     * unit.  This pair is the extent tree's key, so it must describe the whole
     * extent and not the part this file uses. */
    bt_put_le64(&ed.disk_bytenr, data_bytenr);
    bt_put_le64(&ed.disk_num_bytes, BITTER_BLOCK_SIZE);

    /* This file's window into it: all of the content, from the beginning.
     * offset is non-zero only after a partial overwrite or a reflink, neither
     * of which mkfs can produce. */
    bt_put_le64(&ed.offset, 0);
    bt_put_le64(&ed.num_bytes, (bt_u64) content_len);

    ed.type = BITTER_FILE_EXTENT_REG;

    /* The key's offset is a FILE offset: this item describes the run
     * beginning at byte 0 of the file. */
    memset(&key, 0, sizeof key);
    key.objectid = FIRST_FILE_INO;
    key.type     = BITTER_EXTENT_DATA;
    key.offset   = 0;
    bitter_leaf_insert(buf, &key, 6, &ed, sizeof ed);
  }
}
