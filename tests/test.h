/*
 * Assertions and fixtures for core/'s unit tests (tier 1 of docs/skeleton.tex).
 *
 * These run in userspace against core/ compiled for the host, which is the
 * entire point of the seam: a B-tree bug found here costs thirty seconds,
 * where the same bug found by insmod costs a panic and a reboot.
 *
 * Header-only.  Every test is one .c file with a main(), so there is nothing
 * to link and no build order to get wrong.
 */
#ifndef BITTER_TEST_H
#define BITTER_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#include "core/items.h"
#include "core/btree.h"
#include "core/trans.h"
#include "core/fs.h"
#include "env_user.h"
#include "util.h"
#include "core/bitter_crc32c.h"

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */

extern int bt_test_failures;
extern const char *bt_test_name;

/*
 * CHECK does not abort.  A test that stops at the first failure tells you one
 * thing; one that runs to the end tells you the shape of the breakage -- which
 * of twenty inserts went wrong, and whether the rest recovered.
 */
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
      bt_test_failures++;                                                \
    }                                                                    \
  } while (0)

/* As CHECK, with a printf-style note -- for loops, where the line number
 * alone does not say which iteration failed. */
#define CHECK_MSG(cond, ...)                                             \
  do {                                                                   \
    if (!(cond)) {                                                       \
      printf("  FAIL  %s:%d  %s   (", __FILE__, __LINE__, #cond);        \
      printf(__VA_ARGS__);                                               \
      printf(")\n");                                                     \
      bt_test_failures++;                                                \
    }                                                                    \
  } while (0)

#define CHECK_EQ(got, want)                                              \
  do {                                                                   \
    long long g_ = (long long)(got), w_ = (long long)(want);             \
    if (g_ != w_) {                                                      \
      printf("  FAIL  %s:%d  %s: got %lld, want %lld\n",                 \
             __FILE__, __LINE__, #got, g_, w_);                          \
      bt_test_failures++;                                                \
    }                                                                    \
  } while (0)

/* Every block operation should leave the block valid.  Call it after each
 * mutation rather than at the end: the alternative is finding a broken leaf
 * three inserts later with the evidence gone. */
#define CHECK_LEAF(blk)  CHECK_EQ(bitter_leaf_check(blk), 0)
#define CHECK_NODE(blk)  CHECK_EQ(bitter_node_check(blk), 0)

#define TEST_BEGIN(name)  do { bt_test_name = (name); printf("%s\n", (name)); } while (0)
#define TEST_END()        return bt_test_report()

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

/* A scratch image plus an environment over it, so a test can exercise the real
 * seam rather than bare memory.  Blocks 0x10000 and 0x11000 mirror what mkfs
 * lays down; 0x12000 onward is free. */
/* Free ranges a fixture can describe.  Tests seed one and never free, so this
 * only has to be non-zero; extent_build_free_map is not involved. */
#define BT_FIXTURE_FREE_CAP 16

/* Delayed refs a fixture's transaction can hold.  One per block allocated,
 * and nothing drains the set mid-test, so this has to cover every allocation
 * a single test makes -- splits and copy-on-writes included. */
#define BT_FIXTURE_REF_CAP 512

struct bt_fixture {
  int                fd;
  char               path[64];
  struct bitter_env  env;
  struct env_user    priv;
  struct bitter_root root;

  /* The filesystem the root belongs to.  root.fs_info points HERE, so a
   * fixture must not be copied by value -- the copy's root would point into
   * the original.  Every test holds one and passes &f. */
  struct bitter_fs_info      fs;
  struct bitter_free_extent  free_arr[BT_FIXTURE_FREE_CAP];
  struct bitter_delayed_ref  ref_arr[BT_FIXTURE_REF_CAP];

  /* Every mutating call needs one.  bt_fixture_open opens it at generation 2,
   * since mkfs writes 1 -- so fixture blocks look like the previous
   * transaction's, which is what makes them CoW candidates. */
  struct bitter_trans trans;
};

/* Convenience: a key with only the objectid varying, which is all most tests
 * need -- ordering is what is under test, not the key's meaning. */
static inline struct bitter_key_cpu bt_key(bt_u64 objectid)
{
  struct bitter_key_cpu k;
  k.objectid = objectid;
  k.type     = BITTER_INODE_ITEM;
  k.offset   = 0;
  return k;
}

/*
 * Hands the allocator a single free range running from `start` to the end of
 * the device, and nothing else.
 *
 * Replaces the `f->root.next_free = X` the tests used to write: same intent --
 * "allocate from here" -- expressed to the free-space map rather than to a
 * bump cursor.  Blocks below `start` are the ones the fixture built by hand,
 * so handing them out would overwrite the tree under test.
 */
void bt_fixture_free_from(struct bt_fixture *f, bt_u64 start);

int  bt_test_report(void);

/* Checked block write.  pwrite is declared warn_unused_result, and a fixture
 * built on a short write fails later in a way that looks like a tree bug.
 *
 * Also where a fabricated block gets its checksum -- the single point at which
 * one reaches the image, mirroring put_block.  Not in bt_build_leaf and
 * bt_build_node: those also serve items.c tests that never touch a disk, and a
 * csum stamped there would go stale the moment a caller edited the block. */
static inline void wr(int fd, const void *blk, bt_u64 off)
{
  unsigned char tmp[BITTER_BLOCK_SIZE];
  memcpy(tmp, blk, BITTER_BLOCK_SIZE);
  bt_put_le32(((struct bitter_header *)tmp)->csum,
              bt_block_csum(tmp, BITTER_BLOCK_SIZE));
  if (pwrite(fd, tmp, BITTER_BLOCK_SIZE, (off_t)off) != BITTER_BLOCK_SIZE)
    CHECK_MSG(0, "pwrite at 0x%llx failed", (unsigned long long)off);
}
/* The fixture's fsid.  Fixed rather than random so two runs produce
 * byte-identical images; exposed so a test can build extra blocks that
 * belong to the same filesystem. */
extern const bt_u8 bt_test_fsid[BITTER_FSID_SIZE];

void bt_fixture_open(struct bt_fixture *f, const char *name, bt_u64 size);

/*
 * The same, but the image is a REAL filesystem: a superblock and an empty root
 * leaf written by mkfs's own formatters, then read back through
 * bitter_read_super so f->root is reconstructed from disk rather than assigned.
 * f->trans comes from trans_start, so its generation is 2 because mkfs wrote 1.
 *
 * Use this wherever a test needs to commit or reopen -- bt_fixture_open builds
 * a tree in a bare file with no superblock anywhere, which is fine for
 * exercising btree.c and useless for exercising a commit.
 */
void bt_fixture_mkfs(struct bt_fixture *f, const char *name, bt_u64 size);
void bt_fixture_close(struct bt_fixture *f);

/* Writes an empty leaf at `bytenr` and points the fixture's root at it -- the
 * single-leaf tree mkfs produces. */
void bt_make_empty_tree(struct bt_fixture *f, bt_u64 bytenr);

/* Fills a bare 4096-byte buffer with a leaf holding `n` items whose keys are
 * `first`, `first+step`, ... and whose payloads are `size` bytes of `fill`.
 * No environment involved: for testing items.c in isolation. */
void bt_build_leaf(void *blk, bt_u64 bytenr, bt_u32 n, bt_u64 first, bt_u64 step,
                   bt_u32 size, unsigned char fill);

/* The same for a node: `n` children with the given keys and block addresses. */
void bt_build_node(void *blk, bt_u64 bytenr, bt_u8 level, bt_u32 n,
                   const bt_u64 *keys, const bt_u64 *children);

/*
 * Walks the whole tree from the root and checks what no single-block checker
 * can: that every separator in every node equals its child's first key, that
 * levels decrease by exactly one, and that keys ascend across leaf boundaries.
 *
 * This is the check that catches a missing fixup_low_keys -- every block stays
 * individually valid and only the relationship between two of them is wrong.
 * Returns the number of problems found and prints each.
 */
int bt_check_tree(struct bt_fixture *f);

/* Number of items across every leaf, and optionally the keys in order, so a
 * test can assert that everything inserted is still reachable. */
bt_u32 bt_collect_keys(struct bt_fixture *f, bt_u64 *out, bt_u32 max);

#endif
