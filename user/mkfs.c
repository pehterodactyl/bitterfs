/*
 * mkfs.bitterfs — writes an empty bitterfs onto a device or image file.
 *
 *     0x10000  superblock       512 bytes of fields + 3584 zero bytes
 *     0x11000  root tree root   an empty leaf: level 0, nritems 0
 *
 * Write order: root tree block first, superblock last.  The superblock is
 * never copy-on-written, so overwriting it is what makes a transaction real —
 * nothing it points at may still be unwritten when it lands.  Every commit
 * from phase 3 on obeys the same rule.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <linux/fs.h>

#include "format/bitterfs_format.h"
#include "core/bitter_endian.h"
#include "core/bitter_crc32c.h"
#include "util.h"
#include "format.h"
#include "print.h"
#include "core/root.h"
#include "core/extent.h"


/* What the one file contains.  Deliberately short and deliberately not a
 * whole block: it is the difference between size and nbytes, and the thing
 * read_folio has to zero-fill past. */
#define INITIAL_CONTENT "hello from bitterfs\n"

#define INITIAL_ROOT_OFFSET   (BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE)
#define INITIAL_EXTENT_OFFSET (INITIAL_ROOT_OFFSET + BITTER_BLOCK_SIZE)

/* One past the last block mkfs writes: what super.bytes_used records and what
 * bitter_read_super restores into root->next_free.  Derived from the LAST
 * block laid down, not the root -- a mark that lands on a live block is an
 * allocator that hands it out again. */
#define INITIAL_FS_OFFSET     (INITIAL_EXTENT_OFFSET + BITTER_BLOCK_SIZE)

/* The first block that is NOT a tree block: raw file contents, with no
 * bitter_header and no checksum.  Nothing points at it with a key_ptr -- it is
 * reached through an EXTENT_DATA item's payload -- so fsck's tree walk never
 * visits it and must be told about it separately. */
#define INITIAL_DATA_OFFSET   (INITIAL_FS_OFFSET + BITTER_BLOCK_SIZE)

#define INITIAL_BYTES_USED    (INITIAL_DATA_OFFSET + BITTER_BLOCK_SIZE)

static bt_u64 device_size(int fd, const char *path) {
  struct stat st;
  bt_u64 size;

  if (fstat(fd, &st) < 0) {
    die_errno("fstat %s", path);
  }

  /* S_ISREG/S_ISBLK are macros taking the mode, not bit flags: the file type
   * is a 4-bit field, so the test is (mode & S_IFMT) == S_IFREG. */
  if (S_ISREG(st.st_mode)) {
    size = (bt_u64)st.st_size;
  } else if (S_ISBLK(st.st_mode)) {
    /* st_size is 0 for a block device -- the inode describes the device node,
     * not the disk behind it, so the size has to come from the block layer.
     * A failed ioctl leaves `size` untouched, hence the check. */
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
      die_errno("ioctl BLKGETSIZE64 %s", path);
    }
  } else {
    /* A directory, fifo, socket or character device -- the wrong path.  die,
     * not die_errno: nothing failed, so errno is stale. */
    die("%s is not a regular file or block device", path);
  }

  /* Derived from the format rather than hardcoded, so it follows if the
   * superblock moves or the block size changes. */
  if (size < BITTER_SUPER_OFFSET + 2 * BITTER_BLOCK_SIZE) {
    die("%s is too small: %" PRIu64 " bytes, need at least %" PRIu64,
        path, size,
        (bt_u64)(BITTER_SUPER_OFFSET + 2 * BITTER_BLOCK_SIZE));
  }

  return size;
}

/*
 * pwrite rather than lseek + write, so there is no file-position state to get
 * out of step between two calls.
 */
static void write_block(int fd, const void* buf, bt_u64 bytenr, const char* path) {
  ssize_t n = pwrite(fd, buf, BITTER_BLOCK_SIZE, (off_t)bytenr);
  if (n < 0) {
    die_errno("write block at %#" PRIx64 " in %s", bytenr, path);
  }
  /* A separate check: pwrite may legally write fewer bytes and return that
   * count, which is not -1 and so passes the test above.  A partial block is a
   * corrupt image, and retrying gains nothing. */
  if (n != BITTER_BLOCK_SIZE) {
    die("short write at %#" PRIx64 " in %s: %zd of %d bytes",
        bytenr, path, n, BITTER_BLOCK_SIZE);
  }
}


int main(int argc, char* argv[]) {
  /* First, so any die() below is already prefixed. */
  set_progname(argv[0]);

  /* "" rather than NULL: it IS the unlabelled case, and format_super does
   * memcpy(..., label, strlen(label)) -- which with NULL would be undefined
   * even at length zero. */
  const char* label = "";
  int opt;

  /* The loop only STORES.  Acting here would apply every -L rather than the
   * last, and the device path is not known until optind is final.
   *
   * No -U yet: an option with no parser would silently ignore a UUID somebody
   * pinned deliberately, which is worse than not offering it. */
  while ((opt = getopt(argc, argv, "L:h")) != -1) {
    switch (opt) {
      case 'L':
        label = optarg;
        break;
      case 'h':
        exit(EXIT_SUCCESS);
    }
  }

  /* Exactly one operand.  Zero means no device; more usually means a glob
   * matched, and formatting the first would be a bad guess.  Must precede the
   * argv[optind] read below -- argv[argc] is the NULL terminator. */
  if (argc - optind != 1) {
    die("usage: %s [-L label] device", get_progname());
  }
  const char *path = argv[optind];

  /* >=, not >: the field is 32 bytes and NUL-terminated, so 31 characters is
   * the limit.  Checked here rather than in format_super so the message can
   * name the argument -- which is why format_super needs no error path. */
  if (strlen(label) >= BITTER_LABEL_SUPER_SIZE) {
    die("label too long: maximum %d characters", BITTER_LABEL_SUPER_SIZE - 1);
  }

  /* No O_CREAT: a typo'd path would otherwise create a regular file, format
   * it, and report success while the real device went untouched.  O_EXCL on a
   * BLOCK device means "fail if anything else has it open", which is what
   * stops this reformatting a mounted disk. */
  int fd = open(path, O_RDWR | O_EXCL);
  if (fd < 0) {
    die_errno("open %s", path);
  }
  bt_u64 size = device_size(fd, path);

  bt_u8 fsid[BITTER_FSID_SIZE];
  /* A short read would leave the tail as stack garbage -- a plausible UUID
   * that differs between runs for no visible reason. */
  ssize_t rand = getrandom(fsid, BITTER_FSID_SIZE, 0);
  if (rand != BITTER_FSID_SIZE) {
    die_errno("getrandom returned %zd of %d bytes", rand, BITTER_FSID_SIZE);
  }
  /* RFC 4122: version 4 in the high nibble of byte 6, variant 10x in the top
   * two bits of byte 8 -- the same two lines as the kernel's
   * generate_random_uuid (lib/uuid.c:33).  Costs 6 bits of the 128 and makes
   * the result a conforming UUID rather than 16 bytes that merely print like
   * one. */
  fsid[6] = (fsid[6] & 0x0F) | 0x40;
  fsid[8] = (fsid[8] & 0x3F) | 0x80;

  /* One buffer per tree block: the root tree's leaf has to stay intact while
   * the extent tree's is built, because the root item goes into it. */
  bt_u8 buf[BITTER_BLOCK_SIZE];

  format_empty_leaf(buf, fsid, INITIAL_ROOT_OFFSET,
                    1, BITTER_ROOT_TREE_OBJECTID);
  
  bt_u8 extent_buf[BITTER_BLOCK_SIZE];
  memset(extent_buf, 0, BITTER_BLOCK_SIZE);
  struct bitter_root_item it;
  memset(&it, 0, sizeof(struct bitter_root_item));
  format_tree(extent_buf, &it, INITIAL_EXTENT_OFFSET, 1, fsid,
              BITTER_EXTENT_TREE_OBJECTID);

  /* The FS tree: one leaf holding one item, the root directory's inode.  That
   * is a complete and valid empty directory -- a directory's entries are
   * separate DIR_ITEM and DIR_INDEX items keyed under its own objectid, so
   * having none means having no items. */
  bt_u8 fs_buf[BITTER_BLOCK_SIZE];
  struct bitter_root_item fs_it;
  memset(&fs_it, 0, sizeof(struct bitter_root_item));
  format_tree(fs_buf, &fs_it, INITIAL_FS_OFFSET, 1, fsid,
              BITTER_FS_TREE_OBJECTID);

  /* The FS tree's contents: a root directory holding one empty file, so that
   * lookup and readdir have something to find.  Four items -- see
   * format_initial_fs_tree. */
  format_initial_fs_tree(fs_buf, "hello.txt", INITIAL_CONTENT,
                         INITIAL_DATA_OFFSET);

  bt_put_le32(((struct bitter_header*)fs_buf)->csum,
              bt_block_csum(fs_buf, BITTER_BLOCK_SIZE));
  write_block(fd, fs_buf, INITIAL_FS_OFFSET, path);

  /*
   * The file's contents.  Unlike every other block mkfs writes this is NOT a
   * tree block: no header, no checksum, no fsid -- just bytes, zero-padded to
   * the block, because an extent is allocated whole and the tail beyond `size`
   * must read as zeroes rather than as whatever was on the device.
   */
  {
    bt_u8 data_buf[BITTER_BLOCK_SIZE];
    memset(data_buf, 0, BITTER_BLOCK_SIZE);
    memcpy(data_buf, INITIAL_CONTENT, strlen(INITIAL_CONTENT));
    write_block(fd, data_buf, INITIAL_DATA_OFFSET, path);
  }

  struct bitter_key_cpu key;
  //superblock
  bitter_extent_key(&key, 0, BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE);
  struct bitter_extent_item ext_it;
  bt_put_le64(&ext_it.refs, 1);
  /* Reachable from no tree, and that is not a leak: fsck needs to be told the
   * difference, because from the outside they look identical. */
  bt_put_le64(&ext_it.flags, BITTER_EXTENT_FLAG_RESERVED);
  bitter_leaf_insert(extent_buf, &key, 0, &ext_it, sizeof(struct bitter_extent_item));

  /* The two tree blocks below ARE reachable -- one from the superblock, one
   * from its root item -- so they carry no flag. */
  bt_put_le64(&ext_it.flags, 0);
  //root block
  bitter_extent_key(&key, INITIAL_ROOT_OFFSET, BITTER_BLOCK_SIZE);
  bitter_leaf_insert(extent_buf, &key, 1, &ext_it, sizeof(struct bitter_extent_item));
  //extent block
  bitter_extent_key(&key, INITIAL_EXTENT_OFFSET, BITTER_BLOCK_SIZE);
  bitter_leaf_insert(extent_buf, &key, 2, &ext_it, sizeof(struct bitter_extent_item));

  /* fs tree block.  Slot 3 and not any other: extent items are keyed by
   * ADDRESS, and bitter_leaf_insert takes the slot it is given and does not
   * re-sort, so these slots ARE the address ordering written out by hand. */
  bitter_extent_key(&key, INITIAL_FS_OFFSET, BITTER_BLOCK_SIZE);
  bitter_leaf_insert(extent_buf, &key, 3, &ext_it, sizeof(struct bitter_extent_item));

  /* The data block.  Referenced once, by the EXTENT_DATA item in the FS tree
   * rather than by any key_ptr -- the extent tree does not care which kind of
   * reference it is, only that there is one. */
  bitter_extent_key(&key, INITIAL_DATA_OFFSET, BITTER_BLOCK_SIZE);
  bitter_leaf_insert(extent_buf, &key, 4, &ext_it, sizeof(struct bitter_extent_item));

  bt_put_le32(((struct bitter_header*)extent_buf)->csum, 
          bt_block_csum(extent_buf, BITTER_BLOCK_SIZE));
  
  write_block(fd, extent_buf, INITIAL_EXTENT_OFFSET, path);
  memset(&key, 0, sizeof(struct bitter_key_cpu));
  bitter_root_key(&key, BITTER_EXTENT_TREE_OBJECTID);

  bitter_leaf_insert(buf, &key, 0, &it, sizeof(struct bitter_root_item));

  /* Slot 1, after the extent tree's: root items are keyed by the tree's
   * objectid, and BITTER_FS_TREE_OBJECTID (4) sorts after
   * BITTER_EXTENT_TREE_OBJECTID (2).  This is the item fill_super looks for,
   * and until it existed every mount ended at -BITTER_ENOENT. */
  bitter_root_key(&key, BITTER_FS_TREE_OBJECTID);
  bitter_leaf_insert(buf, &key, 1, &fs_it, sizeof(struct bitter_root_item));

  /* format_empty_leaf checksummed an EMPTY leaf; the inserts above invalidated
   * it.  Without this the root tree's first read fails its csum and the
   * filesystem is unmountable from the moment it is created. */
  bt_put_le32(((struct bitter_header *)buf)->csum,
              bt_block_csum(buf, BITTER_BLOCK_SIZE));

  write_block(fd, buf, INITIAL_ROOT_OFFSET, path);

  /* THE ordering constraint, and the reason for two fsyncs.  The superblock
   * must not reach the disk before the block it points at, or a crash leaves a
   * superblock referencing a block that was never written -- a filesystem that
   * looks valid and is not.
   *
   * Nobody is going to crash mid-mkfs and care.  It is here because it is the
   * identical rule every commit obeys from phase 3, and meeting it where there
   * are three blocks and two pointers is easier than meeting it first inside
   * the transaction code. */
  if (fsync(fd) == -1) {
    die_errno("fsync %s", path);
  }

  

  format_super(buf, fsid, label, size, INITIAL_ROOT_OFFSET, INITIAL_BYTES_USED);
  write_block(fd, buf, BITTER_SUPER_OFFSET, path);

  /* mkfs finishing means the filesystem exists, not that it is in page cache. */
  if(fsync(fd) == -1) {
    die_errno("fsync %s", path);
  }
  /* Checked because some filesystems report deferred write errors here. */
  if (close(fd) == -1) {
    die_errno("close %s", path);
  }

  printf("Created bitterfs on %s\n", path);
  print_str ("label",      label[0] ? label : "(none)");
  print_uuid("fsid",       fsid);
  print_u32 ("block size", BITTER_BLOCK_SIZE);
  print_size("total bytes", size);
  print_size("bytes used",  8192);
  print_hex64("superblock", BITTER_SUPER_OFFSET);
  print_hex64("root tree",  BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE);

  return EXIT_SUCCESS;
}
