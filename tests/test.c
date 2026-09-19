/* Fixtures declared in test.h.  See that file for what each is for. */
#include "test.h"
#include "format.h"
#include "trans.h"

int         bt_test_failures = 0;
const char *bt_test_name     = "(unnamed)";

int bt_test_report(void)
{
  if (bt_test_failures) {
    printf("\n%s: %d FAILURE%s\n", bt_test_name, bt_test_failures,
           bt_test_failures == 1 ? "" : "S");
    return 1;
  }
  printf("\n%s: ok\n", bt_test_name);
  return 0;
}

void bt_fixture_open(struct bt_fixture *f, const char *name, bt_u64 size)
{
  snprintf(f->path, sizeof f->path, "/tmp/bt_%s.img", name);
  f->fd = open(f->path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (f->fd < 0) { perror("open"); exit(2); }
  if (ftruncate(f->fd, (off_t)size) < 0) { perror("ftruncate"); exit(2); }
  bitter_env_user_init(&f->env, &f->priv, f->fd, f->path);
  f->root.bytenr      = 0;
  f->root.level       = 0;
  f->root.objectid    = BITTER_ROOT_TREE_OBJECTID;
  f->root.next_free   = 0;
  f->root.total_bytes = size;
  /* Matches what bt_make_empty_tree and the builders write, and is older than
   * f->trans, so fixture blocks are CoW candidates rather than already-owned. */
  f->root.generation  = 0;
  f->trans.generation = 2;

  /* Every allocation records one of these, so a transaction without somewhere
   * to put them fails with -BITTER_ENOMEM on its first block. */
  f->trans.refs      = f->ref_arr;
  f->trans.ref_cap   = BT_FIXTURE_REF_CAP;
  f->trans.ref_count = 0;

  /* The filesystem behind the root.  Without the back-pointer every
   * allocation dereferences garbage inside bitter_alloc_block. */
  /* Zeroed first: extent_root in particular must not be stack garbage, since
   * trans_commit reads its bytenr to decide whether there is an extent tree
   * to record at all. */
  memset(&f->fs, 0, sizeof f->fs);
  f->fs.env         = &f->env;
  f->fs.total_bytes = size;
  f->fs.free        = f->free_arr;
  f->fs.free_cap    = BT_FIXTURE_FREE_CAP;
  f->fs.free_count  = 0;
  f->root.fs_info   = &f->fs;

  /* Empty on purpose: a test that allocates without calling
   * bt_fixture_free_from gets -BITTER_ENOSPC, which is a clear failure rather
   * than a block handed out on top of the fixture's own tree. */
}

void bt_fixture_free_from(struct bt_fixture *f, bt_u64 start)
{
  f->fs.free[0].start  = start;
  f->fs.free[0].length = f->fs.total_bytes - start;
  f->fs.free_count     = 1;
  /* Kept in step while both exist: bitter_read_super still restores it, and
   * unit/commit still checks that it round-trips through the superblock. */
  f->root.next_free    = start;
}

/*
 * Fixed rather than random, unlike mkfs: two runs of a test should produce
 * byte-identical images, which is what makes a golden-image diff possible and
 * a crash reproducible.
 */
const bt_u8 bt_test_fsid[BITTER_FSID_SIZE] = {
  0xb1, 0x77, 0xe4, 0xf5, 0x00, 0x11, 0x40, 0x22,
  0x83, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa
};

void bt_fixture_mkfs(struct bt_fixture *f, const char *name, bt_u64 size)
{
  bt_fixture_open(f, name, size);

  unsigned char blk[BITTER_BLOCK_SIZE];
  bt_u64 root_bytenr = BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE;

  /* Raw pwrite, not wr(): these two carry checksums their own formatters
   * computed, and wr() would recompute a BLOCK-wide one over the superblock,
   * whose checksum covers only its first 512 bytes. */
  format_empty_leaf(blk, bt_test_fsid, root_bytenr, 1, BITTER_ROOT_TREE_OBJECTID);
  if (pwrite(f->fd, blk, sizeof blk, (off_t)root_bytenr) != BITTER_BLOCK_SIZE) {
    perror("pwrite root"); exit(2);
  }
  format_super(blk, bt_test_fsid, "", size, root_bytenr,
               root_bytenr + BITTER_BLOCK_SIZE);
  if (pwrite(f->fd, blk, sizeof blk, (off_t)BITTER_SUPER_OFFSET) != BITTER_BLOCK_SIZE) {
    perror("pwrite super"); exit(2);
  }

  /* Through the real paths, so the fixture proves them rather than assuming
   * them: a bad superblock fails here instead of somewhere downstream. */
  int r = bitter_read_super(&f->env, &f->root);
  if (r) { printf("  FAIL  bt_fixture_mkfs: read_super -> %d\n", r); bt_test_failures++; }
  /* Free space starts past the two blocks written above, exactly as mkfs
   * leaves it.  bitter_read_super restored root.next_free to the same value
   * from super.bytes_used; this is the map saying it too. */
  bt_fixture_free_from(f, root_bytenr + BITTER_BLOCK_SIZE);

  r = trans_start(&f->env, &f->trans);
  if (r) { printf("  FAIL  bt_fixture_mkfs: trans_start -> %d\n", r); bt_test_failures++; }
}

void bt_fixture_close(struct bt_fixture *f)
{
  /* A lost write is invisible to every other assertion in a test: the change
   * is in the buffer the test just read back, and only fails once the block
   * is re-read.  Checking here catches it while the test still has a name. */
  if (f->priv.io_error) {
    printf("  FAIL  io_error %d on %s\n", f->priv.io_error, f->path);
    bt_test_failures++;
  }
  close(f->fd);
  unlink(f->path);
}

void bt_make_empty_tree(struct bt_fixture *f, bt_u64 bytenr)
{
  unsigned char blk[BITTER_BLOCK_SIZE];
  memset(blk, 0, sizeof blk);
  /* level 0, nritems 0 -- both already zero, written for the same reason
   * format_empty_leaf writes them: the field list should match the struct. */
  ((struct bitter_header *)blk)->level = 0;
  bt_put_le64(&((struct bitter_header *)blk)->bytenr, bytenr);
  bt_put_le32(&((struct bitter_header *)blk)->nritems, 0);
  wr(f->fd, blk, bytenr);
  f->root.bytenr = bytenr;
  f->root.level  = 0;
  /* First block past the superblock's and the root's, mirroring what mkfs
   * leaves free.  Unset, a split would allocate block 0 and overwrite the
   * partition table area. */
  bt_fixture_free_from(f, bytenr + BITTER_BLOCK_SIZE);
}

void bt_build_leaf(void *blk, bt_u64 bytenr, bt_u32 n, bt_u64 first, bt_u64 step,
                   bt_u32 size, unsigned char fill)
{
  unsigned char payload[BITTER_MAX_ITEM_SIZE];
  memset(blk, 0, BITTER_BLOCK_SIZE);
  ((struct bitter_header *)blk)->level = 0;
  bt_put_le64(&((struct bitter_header *)blk)->bytenr, bytenr);
  memset(payload, fill, size);
  for (bt_u32 i = 0; i < n; i++) {
    struct bitter_key_cpu k = bt_key(first + (bt_u64)i * step);
    bitter_leaf_insert(blk, &k, i, payload, size);
  }
}

void bt_build_node(void *blk, bt_u64 bytenr, bt_u8 level, bt_u32 n,
                   const bt_u64 *keys, const bt_u64 *children)
{
  struct bitter_header *h = blk;
  memset(blk, 0, BITTER_BLOCK_SIZE);
  h->level = level;
  bt_put_le64(&h->bytenr, bytenr);
  bt_put_le32(&h->nritems, n);
  for (bt_u32 i = 0; i < n; i++) {
    struct bitter_key_ptr *kp = bitter_node_key_ptr(blk, i);
    struct bitter_key_cpu  k  = bt_key(keys[i]);
    bitter_key_to_disk(&kp->key, &k);
    bt_put_le64(&kp->blockptr, children[i]);
    /* Must mirror the child's own header.generation, which btree_search now
     * cross-checks.  Zero, because that is what the builders leave in a header
     * they memset -- the 1 that used to be here described a convention the
     * fixtures never actually followed. */
    bt_put_le64(&kp->generation, 0);
  }
}

/* --- whole-tree walk ---------------------------------------------- */

struct walk_state {
  struct bt_fixture *f;
  int      problems;
  int      have_prev;
  bt_u64   prev_key;      /* last key seen, for the across-leaves ordering */
  bt_u64  *out;           /* optional key collection */
  bt_u32   out_max, out_n;
};

static void walk(struct walk_state *w, bt_u64 bytenr, bt_u8 level,
                 const struct bitter_key_cpu *expect_first)
{
  struct bitter_buf *buf = w->f->env.ops->read_block(&w->f->env, bytenr);
  if (!buf) {
    printf("  FAIL  unreadable block at %#" PRIx64 "\n", bytenr);
    w->problems++;
    return;
  }
  const struct bitter_header *h = buf->b_data;

  if (h->level != level) {
    printf("  FAIL  block %#" PRIx64 ": level %u, parent expected %u\n",
           bytenr, h->level, level);
    w->problems++;
  }

  bt_u32 n = bitter_leaf_nritems(buf->b_data);
  bt_u32 bad = level ? bitter_node_check(buf->b_data)
                     : bitter_leaf_check(buf->b_data);
  if (bad) {
    printf("  FAIL  block %#" PRIx64 ": %s_check = 0x%x\n",
           bytenr, level ? "node" : "leaf", bad);
    w->problems++;
  }

  /* The separator its parent holds must equal this block's own first key.
   * Neither checker can see this -- both blocks are individually valid, and
   * only the relationship between them is wrong.  A missing fixup_low_keys
   * shows up here and nowhere else. */
  if (expect_first && n > 0) {
    struct bitter_key_cpu first;
    if (level) {
      bitter_key_from_disk(&first, &bitter_node_key_ptr(buf->b_data, 0)->key);
    } else {
      bitter_key_from_disk(&first, &bitter_leaf_item(buf->b_data, 0)->key);
    }
    if (bitter_key_cmp(&first, expect_first) != 0) {
      printf("  FAIL  block %#" PRIx64 ": parent separator says %" PRIu64
             ", block starts at %" PRIu64 "\n",
             bytenr, expect_first->objectid, first.objectid);
      w->problems++;
    }
  }

  if (level == 0) {
    for (bt_u32 i = 0; i < n; i++) {
      struct bitter_key_cpu k;
      bitter_key_from_disk(&k, &bitter_leaf_item(buf->b_data, i)->key);
      /* Ordering ACROSS leaves, which bitter_leaf_check cannot see either. */
      if (w->have_prev && k.objectid <= w->prev_key) {
        printf("  FAIL  key %" PRIu64 " follows %" PRIu64 " across leaves\n",
               k.objectid, w->prev_key);
        w->problems++;
      }
      w->prev_key = k.objectid;
      w->have_prev = 1;
      if (w->out && w->out_n < w->out_max) w->out[w->out_n++] = k.objectid;
    }
  } else {
    for (bt_u32 i = 0; i < n; i++) {
      struct bitter_key_cpu sep;
      bitter_key_from_disk(&sep, &bitter_node_key_ptr(buf->b_data, i)->key);
      walk(w, bitter_node_blockptr(buf->b_data, i), (bt_u8)(level - 1), &sep);
    }
  }

  w->f->env.ops->put_block(&w->f->env, buf);
}

int bt_check_tree(struct bt_fixture *f)
{
  struct walk_state w;
  memset(&w, 0, sizeof w);
  w.f = f;
  walk(&w, f->root.bytenr, f->root.level, 0);
  bt_test_failures += w.problems;
  return w.problems;
}

bt_u32 bt_collect_keys(struct bt_fixture *f, bt_u64 *out, bt_u32 max)
{
  struct walk_state w;
  memset(&w, 0, sizeof w);
  w.f = f; w.out = out; w.out_max = max;
  walk(&w, f->root.bytenr, f->root.level, 0);
  /* Problems found here are reported but not counted twice -- a caller that
   * wants them counted calls bt_check_tree. */
  return w.out_n;
}
