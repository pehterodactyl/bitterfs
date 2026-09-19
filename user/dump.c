/*
 * bitter-dump — reads an image back and prints what is in it.
 *
 * The inverse of mkfs: where mkfs computes checksums and writes fields,
 * this recomputes them and compares.  It prints every check, passing or
 * failing, and exits non-zero if any failed — the golden-image tests in
 * tests/image/ read that status.
 *
 * A failed check never stops the report.  A corrupt block is exactly when you
 * most want to see inside it, and stopping at the first problem is the opposite
 * of useful when you are diffing two images.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#include "format/bitterfs_format.h"
#include "core/bitter_endian.h"
#include "core/bitter_crc32c.h"
#include "util.h"
#include "print.h"


/* No validation here: an I/O failure means there are no bytes to look at, so
 * it is fatal, while a bad checksum is the report's whole content.  Different
 * responses, so different mechanisms. */
static void read_block(int fd, void *buf, bt_u64 bytenr, const char* path) {
  ssize_t n = pread(fd, buf, BITTER_BLOCK_SIZE, (off_t)bytenr);
  if (n < 0) {
    die_errno("read block at %#" PRIx64 " in %s", bytenr, path);
  }
  if (n != BITTER_BLOCK_SIZE) {
    die("short read at %#" PRIx64 " in %s: %zd of %d bytes",
            bytenr, path, n, BITTER_BLOCK_SIZE);
  }
}


/* The checkers return which checks failed and print nothing, so the same
 * functions can be reused where the response is pr_err and -EIO rather than
 * a report.  Distinct bits across both sets, so results can be OR'd. */
#define CSUM_ERROR   1
#define MAGIC_ERROR  2
#define BYTENR_ERROR 4
#define FSID_ERROR   8
#define ROOT_ERROR  16

/* 32..512: the superblock's checksum stops at the end of the struct, because
 * that is the sector the drive writes atomically. */
static int check_super(const void* buf) {
  const struct bitter_super* sb = (const struct bitter_super*) buf;
  int flags = 0;

  if (bt_get_le32(sb->csum) != bt_block_csum(buf, BITTER_SUPER_SIZE)) {
    flags |= CSUM_ERROR;
  }
  /* memcmp, not strcmp: the field is exactly 8 bytes with no NUL. */
  if (memcmp(sb->magic, BITTER_MAGIC, BITTER_MAGIC_SIZE) != 0) {
    flags |= MAGIC_ERROR;
  }
  /* root is about to be used as a file offset, so it is validated before
   * anyone dereferences it — an out-of-range pread would die inside
   * read_block and take the rest of the report with it. */
  bt_u64 root = bt_get_le64(&sb->root);
  if (root < BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE ||
      root >= bt_get_le64(&sb->total_bytes) ||
      root % BITTER_BLOCK_SIZE != 0) {
    flags |= ROOT_ERROR;
  }
  return flags;
}

/*
 * 32..4096: a tree block's items fill the whole block, so a shorter range
 * would leave them unprotected.
 *
 * expect_bytenr and expect_fsid come from OUTSIDE the block on purpose.  A
 * block compared against its own fields always agrees; comparing its claims to
 * the caller's expectation is what catches a misdirected write or a stale
 * block from a previous mkfs — both intact, both correctly checksummed.
 */
static int check_header(const void *buf, bt_u64 expect_bytenr, const bt_u8 *expect_fsid) {
  const struct bitter_header* bh = (const struct bitter_header*) buf;
  int flags = 0;

  if (bt_get_le32(bh->csum) != bt_block_csum(buf, BITTER_BLOCK_SIZE)) {
    flags |= CSUM_ERROR;
  }
  if (bt_get_le64(&bh->bytenr) != expect_bytenr) {
    flags |= BYTENR_ERROR;
  }
  if (memcmp(bh->fsid, expect_fsid, BITTER_FSID_SIZE) != 0) {
    flags |= FSID_ERROR;
  }
  return flags;
}


/*
 * Table-driven so adding a check is one line here and one bit above, with no
 * edit to the loop.  Passing checks print too: a tool that reports only
 * failures leaves you unable to tell "checked and fine" from "never checked".
 */
struct check_desc {
  int          bit;
  const char*  what;
};

static const struct check_desc super_checks[] = {
  { CSUM_ERROR,   "csum verifies over 32..512"   },
  { MAGIC_ERROR,  "magic is " BITTER_MAGIC       },
  { ROOT_ERROR,   "root pointer in range and block-aligned" },
};

static const struct check_desc header_checks[] = {
  { CSUM_ERROR,   "csum verifies over 32..4096"  },
  { BYTENR_ERROR, "bytenr matches where it was read from" },
  { FSID_ERROR,   "fsid matches the superblock"  },
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* print_check takes a PASS value and the bits mean "failed", so the polarity
 * flip happens here rather than at every call site.  Returns non-zero if
 * anything failed. */
static int report_checks(int flags, const struct check_desc* d, size_t n) {
  int bad = 0;
  for (size_t i = 0; i < n; i++) {
    bad |= !print_check(d[i].what, (flags & d[i].bit) == 0);
  }
  return bad;
}


static void print_super(const struct bitter_super* sb) {
  print_section("superblock");
  print_bytes ("csum",        sb->csum, 4);
  print_uuid  ("fsid",        sb->fsid);
  print_bytes ("magic",       sb->magic, BITTER_MAGIC_SIZE);
  print_hex64 ("bytenr",      bt_get_le64(&sb->bytenr));
  print_u64   ("generation",  bt_get_le64(&sb->generation));
  print_hex64 ("root",        bt_get_le64(&sb->root));
  print_size  ("total bytes", bt_get_le64(&sb->total_bytes));
  print_hex64 ("next free",   bt_get_le64(&sb->bytes_used));
  print_hex64 ("compat",      bt_get_le64(&sb->compat_flags));
  print_hex64 ("ro compat",   bt_get_le64(&sb->ro_compat_flags));
  print_hex64 ("incompat",    bt_get_le64(&sb->incompat_flags));
  print_u32   ("block size",  bt_get_le32(&sb->block_size));
  print_u32   ("csum type",   bt_get_le16(&sb->csum_type));
  print_u32   ("root level",  sb->root_level);
  print_str   ("label",       sb->label[0] ? (const char *)sb->label : "(none)");
}

static void print_header(const struct bitter_header* bh) {
  print_section("root tree block");
  print_bytes ("csum",       bh->csum, 4);
  print_uuid  ("fsid",       bh->fsid);
  print_hex64 ("bytenr",     bt_get_le64(&bh->bytenr));
  print_u64   ("generation", bt_get_le64(&bh->generation));
  print_u64   ("owner",      bt_get_le64(&bh->owner));
  print_u32   ("nritems",    bt_get_le32(&bh->nritems));
  print_u32   ("level",      bh->level);
}


int main(int argc, char* argv[]) {
  set_progname(argv[0]);
  if (argc != 2) {
    die("usage: <device>");
  }
  const char* path = argv[1];

  /* Read-only and no O_EXCL, unlike mkfs: dump must be able to inspect a
   * MOUNTED filesystem, which is when you most want to look at one. */
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    die_errno("open %s", path);
  }

  bt_u8 buf[BITTER_BLOCK_SIZE];
  int bad = 0;

  read_block(fd, buf, BITTER_SUPER_OFFSET, path);
  const struct bitter_super* sb = (const struct bitter_super*) buf;
  print_super(sb);
  print_section("superblock checks");
  int super_flags = check_super(buf);
  bad |= report_checks(super_flags, super_checks, ARRAY_LEN(super_checks));

  /* Copied out BEFORE buf is reused for the tree block.  sb points into buf,
   * so reading sb->fsid after the next read_block would compare the tree
   * block's fsid against itself — a check that can never fail. */
  bt_u64 root_bytenr = bt_get_le64(&sb->root);
  bt_u8  super_fsid[BITTER_FSID_SIZE];
  memcpy(super_fsid, sb->fsid, BITTER_FSID_SIZE);

  if (super_flags & ROOT_ERROR) {
    /* Nothing meaningful at the other end, and read_block would die on an
     * out-of-range pread, losing the rest of the report. */
    printf("\n  root pointer %#" PRIx64 " is unusable -- skipping tree block\n",
           root_bytenr);
  } else {
    read_block(fd, buf, root_bytenr, path);
    print_header((const struct bitter_header*) buf);
    print_section("root tree block checks");
    int root_flags = check_header(buf, root_bytenr, super_fsid);
    bad |= report_checks(root_flags, header_checks, ARRAY_LEN(header_checks));
  }

  close(fd);
  return bad ? EXIT_FAILURE : EXIT_SUCCESS;
}
