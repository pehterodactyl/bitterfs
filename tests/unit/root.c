/*
 * The root tree: finding a tree by objectid, and recording one that moved.
 *
 * Covers the pair in core/root.c -- bitter_find_root decodes a root item into
 * a bitter_root, bitter_update_root encodes one back.  They are tested
 * together because each is the other's only real verifier: an encode nobody
 * decodes proves nothing, and a decode of bytes this test wrote by hand would
 * only prove the test agrees with itself.
 *
 * Builds its own image rather than using bt_fixture_mkfs, which lays down a
 * single tree.  The fixture's root tree is scratch space that other tests fill
 * with arbitrary keys; a root item living in it would collide with them on
 * objectid, so the multi-tree image belongs here.
 */
#include "../test.h"
#include "format.h"
#include "trans.h"
#include "core/root.h"
#include "core/bitter_crc32c.h"

#define ROOT_BYTENR   (BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE)
#define EXTENT_BYTENR (ROOT_BYTENR + BITTER_BLOCK_SIZE)
#define BYTES_USED    (EXTENT_BYTENR + BITTER_BLOCK_SIZE)

/*
 * The three blocks mkfs writes, in the order mkfs writes them.  Deliberately a
 * copy of that sequence rather than a call into it: if mkfs and this diverge,
 * the test should fail rather than quietly follow.
 */
static void build_image(struct bt_fixture *f, bt_u64 size)
{
  unsigned char blk[BITTER_BLOCK_SIZE];
  unsigned char ext[BITTER_BLOCK_SIZE];
  struct bitter_root_item item;
  struct bitter_key_cpu key;

  format_empty_leaf(blk, bt_test_fsid, ROOT_BYTENR, 1, BITTER_ROOT_TREE_OBJECTID);

  format_tree(ext, &item, EXTENT_BYTENR, 1, bt_test_fsid,
                     BITTER_EXTENT_TREE_OBJECTID);
  if (pwrite(f->fd, ext, sizeof ext, (off_t)EXTENT_BYTENR) != BITTER_BLOCK_SIZE) {
    perror("pwrite extent"); exit(2);
  }

  /* The insert invalidates the checksum format_empty_leaf computed over an
   * empty leaf, so the block is re-stamped before it is written. */
  bitter_root_key(&key, BITTER_EXTENT_TREE_OBJECTID);
  bitter_leaf_insert(blk, &key, 0, &item, sizeof item);
  bt_put_le32(((struct bitter_header *)blk)->csum,
              bt_block_csum(blk, BITTER_BLOCK_SIZE));
  if (pwrite(f->fd, blk, sizeof blk, (off_t)ROOT_BYTENR) != BITTER_BLOCK_SIZE) {
    perror("pwrite root"); exit(2);
  }

  format_super(blk, bt_test_fsid, "", size, ROOT_BYTENR, BYTES_USED);
  if (pwrite(f->fd, blk, sizeof blk, (off_t)BITTER_SUPER_OFFSET) != BITTER_BLOCK_SIZE) {
    perror("pwrite super"); exit(2);
  }
}

int main(void)
{
  bt_test_name = "root";
  printf("%s\n", bt_test_name);

  const bt_u64 size = 64 << 20;
  struct bt_fixture f;
  bt_fixture_open(&f, "root", size);
  build_image(&f, size);

  CHECK_EQ(bitter_read_super(&f.env, &f.root), 0);
  CHECK_EQ(trans_start(&f.env, &f.trans), 0);

  /* build_image wrote three blocks by hand, so free space starts past them. */
  bt_fixture_free_from(&f, BYTES_USED);

  /* --- find ------------------------------------------------------------ */
  struct bitter_root ext;
  memset(&ext, 0xAA, sizeof ext);   /* poison: every field must be written */
  CHECK_EQ(bitter_find_root(&f.env, &f.root, BITTER_EXTENT_TREE_OBJECTID, &ext), 0);
  CHECK_EQ((int)ext.bytenr, (int)EXTENT_BYTENR);
  CHECK_EQ((int)ext.level, 0);
  CHECK_EQ((int)ext.generation, 1);
  CHECK_EQ((int)ext.objectid, BITTER_EXTENT_TREE_OBJECTID);
  /* Device-wide, and not in the item: they can only come from tree_root. */
  CHECK_EQ((int)ext.total_bytes, (int)f.root.total_bytes);
  CHECK_EQ((int)ext.next_free, (int)f.root.next_free);
  printf("  found extent tree: bytenr 0x%llx level %u gen %llu\n",
         (unsigned long long)ext.bytenr, ext.level,
         (unsigned long long)ext.generation);

  /* A tree that was never created is a clean answer, not an error. */
  struct bitter_root missing;
  CHECK_EQ(bitter_find_root(&f.env, &f.root, BITTER_FS_TREE_OBJECTID, &missing),
           -BITTER_ENOENT);

  /* --- update ---------------------------------------------------------- */
  /* Stands in for what a real transaction does to the extent tree: its root
   * gets copy-on-written to a fresh block and takes this transaction's
   * generation.  Where that block came from does not matter here -- what is
   * under test is whether the root tree learns about it. */
  bt_u64 old_root_tree = f.root.bytenr;
  /* A sentinel well clear of the allocator, so it cannot collide with the
   * block the root tree's own CoW takes and the two are told apart in the
   * output.  Nothing ever reads it -- only the recorded address is under
   * test. */
  bt_u64 moved         = BITTER_SUPER_OFFSET + 0x100000;

  struct bitter_root updated = ext;
  updated.bytenr     = moved;
  updated.generation = f.trans.generation;
  /* Non-zero, and not the low byte of `moved`: with every tree at level 0 an
   * encoder that wrote the wrong field would still produce a 0 here and go
   * unnoticed. */
  updated.level      = 2;

  CHECK_EQ(bitter_update_root(&f.env, &f.root, &updated, &f.trans), 0);

  /* The descent CoWs, so the root tree itself must have moved -- that moved
   * address is what the superblock will record. */
  CHECK_MSG(f.root.bytenr != old_root_tree,
            "root tree did not move: 0x%llx", (unsigned long long)f.root.bytenr);
  CHECK_EQ((int)f.root.generation, (int)f.trans.generation);
  printf("  root tree moved 0x%llx -> 0x%llx on update\n",
         (unsigned long long)old_root_tree, (unsigned long long)f.root.bytenr);

  /* --- find again, in memory ------------------------------------------- */
  struct bitter_root again;
  memset(&again, 0xAA, sizeof again);
  CHECK_EQ(bitter_find_root(&f.env, &f.root, BITTER_EXTENT_TREE_OBJECTID, &again), 0);
  CHECK_EQ((int)again.bytenr, (int)moved);
  CHECK_EQ((int)again.generation, (int)f.trans.generation);
  CHECK_EQ((int)again.level, 2);

  /* --- and after a commit, from a filesystem this process never built ---- */
  CHECK_EQ(trans_commit(&f.fs, &f.trans, &f.root), 0);

  int fd2 = open(f.path, O_RDWR);
  if (fd2 < 0) { perror("reopen"); exit(2); }
  struct env_user p2;
  struct bitter_env e2;
  bitter_env_user_init(&e2, &p2, fd2, f.path);

  struct bitter_root r2, ext2;
  CHECK_EQ(bitter_read_super(&e2, &r2), 0);
  CHECK_MSG(r2.bytenr == f.root.bytenr, "superblock kept the old root tree");
  CHECK_EQ(bitter_find_root(&e2, &r2, BITTER_EXTENT_TREE_OBJECTID, &ext2), 0);
  CHECK_EQ((int)ext2.bytenr, (int)moved);
  CHECK_EQ((int)ext2.generation, (int)f.trans.generation);
  CHECK_EQ((int)ext2.level, 2);

  /* --- the key as it actually sits on disk ------------------------------ */
  /* Everything above goes through bitter_root_key on both sides, so a wrong
   * key type would be written and looked up consistently and never show.
   * This reads the descriptor and pins the format itself. */
  {
    unsigned char leafblk[BITTER_BLOCK_SIZE];
    if (pread(fd2, leafblk, sizeof leafblk, (off_t)r2.bytenr) != BITTER_BLOCK_SIZE) {
      perror("pread root leaf"); exit(2);
    }
    struct bitter_key_cpu on_disk;
    bitter_key_from_disk(&on_disk, &bitter_leaf_item(leafblk, 0)->key);
    CHECK_EQ((int)on_disk.objectid, BITTER_EXTENT_TREE_OBJECTID);
    CHECK_EQ((int)on_disk.type,     BITTER_ROOT_ITEM);
    CHECK_EQ((int)on_disk.offset,   0);
    CHECK_EQ((int)bitter_item_get_size(bitter_leaf_item(leafblk, 0)),
             BITTER_ROOT_ITEM_SIZE);
  }
  printf("  survived commit: extent tree at 0x%llx gen %llu, read back fresh\n",
         (unsigned long long)ext2.bytenr, (unsigned long long)ext2.generation);

  /* --- a root item of the wrong length is refused ----------------------- */
  /* Built as a STRUCTURALLY VALID leaf holding a short item, not by scribbling
   * on a descriptor.  Shrinking a size in place leaves a hole in the payload
   * run, and bitter_leaf_check rejects the block during the descent -- so the
   * test would pass without either size check ever running.  Here the leaf is
   * beyond reproach and the only thing wrong is that a BITTER_ROOT_ITEM key
   * carries 8 bytes instead of 17. */
  unsigned char blk[BITTER_BLOCK_SIZE];
  struct bitter_key_cpu rk;
  unsigned char stub[8] = { 0 };
  bt_u64 leaf = r2.bytenr;

  format_empty_leaf(blk, bt_test_fsid, leaf, r2.generation,
                    BITTER_ROOT_TREE_OBJECTID);
  bitter_root_key(&rk, BITTER_EXTENT_TREE_OBJECTID);
  bitter_leaf_insert(blk, &rk, 0, stub, sizeof stub);
  bt_put_le32(((struct bitter_header *)blk)->csum,
              bt_block_csum(blk, BITTER_BLOCK_SIZE));
  if (pwrite(fd2, blk, sizeof blk, (off_t)leaf) != BITTER_BLOCK_SIZE) {
    perror("pwrite short item"); exit(2);
  }
  CHECK_EQ((int)bitter_leaf_check(blk), 0);   /* the leaf itself is sound */

  struct bitter_env e3; struct env_user p3;
  bitter_env_user_init(&e3, &p3, fd2, f.path);
  struct bitter_root r3, bad;
  CHECK_EQ(bitter_read_super(&e3, &r3), 0);

  /* bitter_update_root below descends with cow set, so it allocates -- which
   * needs a filesystem behind r3.  A root read straight from a superblock has
   * no back-pointer; only bitter_fs_info_init supplies one. */
  struct bitter_fs_info      fs3;
  struct bitter_free_extent  free3[8];
  memset(&fs3, 0, sizeof fs3);
  fs3.env         = &e3;
  fs3.total_bytes = r3.total_bytes;
  fs3.free        = free3;
  fs3.free_cap    = 8;
  fs3.free[0].start  = r3.next_free;
  fs3.free[0].length = r3.total_bytes - r3.next_free;
  fs3.free_count  = 1;
  r3.fs_info      = &fs3;
  CHECK_EQ(bitter_find_root(&e3, &r3, BITTER_EXTENT_TREE_OBJECTID, &bad),
           -BITTER_EUCLEAN);

  /* The write path refuses it too, and that is the half that matters:
   * find_root would merely decode nonsense, while update_root would memcpy 17
   * bytes over an 8-byte item and into its neighbour's payload -- damage no
   * later check could see, because every descriptor would stay consistent. */
  /* Built by hand, so it needs its own delayed-ref array: trans_start sets
   * only the generation, and the descent below allocates. */
  struct bitter_trans t3;
  memset(&t3, 0, sizeof t3);
  t3.refs    = f.ref_arr;
  t3.ref_cap = BT_FIXTURE_REF_CAP;
  CHECK_EQ(trans_start(&e3, &t3), 0);
  CHECK_EQ(bitter_update_root(&e3, &r3, &updated, &t3), -BITTER_EUCLEAN);
  printf("  a root item of the wrong length is refused by both halves (EUCLEAN)\n");

  close(fd2);
  bt_fixture_close(&f);
  return bt_test_report();
}
