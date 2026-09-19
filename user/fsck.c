#include "user/fsck.h"
#include "core/items.h"
#include "core/fs.h"
#include "core/extent.h"
#include "core/bitter_env.h"
#include "core/bitter_endian.h"
#include "user/trans.h"
#include "user/util.h"
#include "user/env_user.h"
#include "user/print.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* The worklist                                                        */
/* ------------------------------------------------------------------ */

/*
 * One block that has been DISCOVERED but not yet examined, and everything the
 * pointer that discovered it claimed about it.
 *
 * The four fields beyond `bytenr` are what let a finding name the pointer
 * rather than only the victim: `generation` and `level` are the parent's
 * claims, checked against the block's own header once it is read, and
 * `parent`/`slot` say where to look.  parent == 0 means the pointer came from
 * outside the tree -- the superblock, or a root item -- and is safe as a
 * sentinel because address 0 lies inside the reserved range mkfs records, so
 * no key_ptr can ever name it.
 */
struct fsck_pending {
  bt_u64 bytenr;
  bt_u64 generation;
  bt_u64 parent;
  bt_u32 slot;
  bt_u8  level;
};

/*
 * The frontier: blocks seen but not yet opened.  Grown by fsck_push and
 * drained by fsck_observe_tree, empty at both ends of a walk.
 *
 * A stack rather than a queue.  Both are correct, since the walk counts rather
 * than scans in order, but depth-first holds roughly fanout x depth entries
 * where breadth-first would hold an entire level.
 */
struct fsck_worklist {
  struct fsck_pending* item;
  bt_size              count;
  bt_size              cap;
};

/*
 * Ceiling on blocks visited in one tree.
 *
 * fsck runs on filesystems that are ALREADY broken -- that is the job -- and a
 * blockptr pointing back at an ancestor would otherwise refill the worklist
 * forever.  No correct tree can hold more blocks than the device has, so
 * exceeding it is itself a finding rather than a reason to hang.
 */
#define FSCK_MAX_VISITS(fs) ((fs) / BITTER_BLOCK_SIZE)

static int fsck_check_header(const struct fsck_pending* p,
        const struct bitter_root* root, void* block);

/*
 * Grows `arr` (element size `esz`) to hold at least one more entry.
 *
 * Doubling, with the empty case named: cap * 2 is 0 when cap is 0, so a
 * zero-initialised array would double forever without ever making room.
 *
 * realloc leaves the original allocation valid when it fails, so the result
 * goes to a temporary -- assigning straight to *arr would drop the only
 * pointer to memory the caller still owns and still frees.
 */
static int fsck_grow(void** arr, bt_size* cap, bt_size count, bt_size esz) {
  if (count < *cap) {
    return 0;
  }
  bt_size want = *cap ? *cap * 2 : 64;
  void* bigger = realloc(*arr, want * esz);
  if (!bigger) {
    return -BITTER_ENOMEM;
  }
  *arr = bigger;
  *cap = want;
  return 0;
}

/*
 * Records one pointer: appends its target to `refs` and schedules the block on
 * `wl`.
 *
 * BOTH, always, and this is the only place either array grows.  That makes
 * "one refs entry per pointer" a property of the code rather than a discipline
 * to remember: no path schedules a block without counting the pointer that led
 * to it, and none counts one without scheduling the block.
 *
 * Both arrays are grown BEFORE either is written.  Appending to refs and then
 * failing to grow the worklist would count a pointer to a block that is never
 * visited, and the comparison pass would report the resulting mismatch as a
 * corrupt refcount when it is really our own bookkeeping.
 *
 * Unlike the arrays in core/, these are owned here and userspace can allocate,
 * so a full array means GROW rather than fail.  -BITTER_ENOMEM is reserved for
 * the allocator genuinely refusing.
 *
 * Does no I/O and validates nothing: it has not seen the block.  Every check
 * happens at the pop, where the contents are in hand.
 */
static int fsck_push(struct fsck_worklist* wl, struct fsck_refs* refs,
          const struct fsck_pending* p) {

  int r = fsck_grow((void**)&refs->bytenr, &refs->cap, refs->count,
                    sizeof *refs->bytenr);
  if (r < 0) {
    return r;
  }
  r = fsck_grow((void**)&wl->item, &wl->cap, wl->count, sizeof *wl->item);
  if (r < 0) {
    return r;
  }

  refs->bytenr[refs->count++] = p->bytenr;
  wl->item[wl->count++]       = *p;
  return 0;
}


/*
 * Records a reference to a block that is NOT a tree block.
 *
 * This deliberately breaks the pairing fsck_push exists to guarantee -- it
 * counts a reference without scheduling a visit -- and the reason is that the
 * target is file DATA.  It has no bitter_header, no checksum and no fsid, so
 * reading it as a tree block would report a corrupt block for every file on
 * the device.  Nothing points at it with a key_ptr either; the reference lives
 * in an EXTENT_DATA item's payload, which is why the tree walk alone never
 * sees it.
 *
 * The extent tree does not distinguish the two kinds: an extent item counts
 * references, not what sort of thing is doing the referencing.  So this entry
 * goes into the same array and is compared by the same pass.
 */
static int fsck_note_data_ref(struct fsck_refs* refs, bt_u64 bytenr) {
  int r = fsck_grow((void**)&refs->bytenr, &refs->cap, refs->count,
                    sizeof *refs->bytenr);
  if (r < 0) {
    return r;
  }
  refs->bytenr[refs->count++] = bytenr;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Walking one tree                                                    */
/* ------------------------------------------------------------------ */

/*
 * Visits every block of one tree, recording a reference for each pointer found
 * and checking each block against what the pointer claimed.
 *
 * Returns the NUMBER of problems found, or a negative error that stopped the
 * walk.  Problems are not errors: a filesystem with a corrupt block is exactly
 * what fsck exists to describe, so a bad block is reported and the walk
 * continues.  Only running out of memory, or exceeding the visit ceiling, ends
 * it early -- in both cases the observations are incomplete and the comparison
 * that follows would invent findings.
 *
 * `seen` accumulates across every tree, because a block's refcount is the
 * number of pointers at it from ANYWHERE.  Counting per tree would let a block
 * referenced once by each look correct in both and wrong in neither.
 *
 * The worklist is local: empty when the walk starts, empty when it ends.
 */
long fsck_observe_tree(struct bitter_env* env, struct bitter_root* root,
          struct fsck_refs* seen) {

  struct fsck_worklist wl;
  memset(&wl, 0, sizeof wl);

  long problems = 0;
  bt_size visits = 0;
  bt_size max_visits = (bt_size)FSCK_MAX_VISITS(root->total_bytes);

  /* The root's own pointer, from outside the tree.  Pushed through the same
   * call as every key_ptr, so even this one is counted by the same code. */
  struct fsck_pending root_pn;
  memset(&root_pn, 0, sizeof root_pn);
  root_pn.bytenr     = root->bytenr;
  root_pn.generation = root->generation;
  root_pn.parent     = 0;
  root_pn.slot       = 0;
  root_pn.level      = root->level;

  int s = fsck_push(&wl, seen, &root_pn);
  if (s < 0) {
    free(wl.item);
    return s;
  }

  while (wl.count != 0) {

    if (++visits > max_visits) {
      printf("  tree %llu: more than %llu blocks visited -- a pointer cycle\n",
             (unsigned long long)root->objectid,
             (unsigned long long)max_visits);
      free(wl.item);
      return -BITTER_EUCLEAN;
    }

    /* Pop.  Order does not matter -- this counts rather than scans -- so the
     * last entry fills the hole and nothing shifts. */
    struct fsck_pending it = wl.item[0];
    wl.item[0] = wl.item[wl.count - 1];
    wl.count--;

    struct bitter_buf* buf = env->ops->read_block(env, it.bytenr);
    if (!buf) {
      /* read_block has already said why on stderr.  A dangling pointer is a
       * finding, not a reason to abandon the rest of the tree -- and the
       * reference was recorded at the push, so the comparison still knows
       * something pointed here. */
      printf("  block %#llx: unreadable, referenced by %s\n",
             (unsigned long long)it.bytenr,
             it.parent ? "a node" : "a root");
      problems++;
      continue;
    }

    void* block = buf->b_data;

    /* Reported, then descended into anyway: a bad node's children may be
     * perfectly good, and stopping here would hide everything beneath it. */
    problems += fsck_check_header(&it, root, block);

    if (it.level != 0) {
      bt_u32 nritems = bitter_leaf_nritems(block);
      for (bt_u32 i = 0; i < nritems; i++) {
        struct fsck_pending child;
        memset(&child, 0, sizeof child);
        child.bytenr     = bitter_node_blockptr(block, i);
        child.generation = bitter_node_generation(block, i);
        child.parent     = it.bytenr;
        child.slot       = i;
        child.level      = (bt_u8)(it.level - 1);

        s = fsck_push(&wl, seen, &child);
        if (s < 0) {
          env->ops->put_block(env, buf);
          free(wl.item);
          return s;
        }
      }
    } else {
      /*
       * A leaf.  Its items are not pointers to blocks -- except EXTENT_DATA,
       * whose payload names a range of the device holding file contents.
       * Those references are invisible to the walk above, so without this the
       * extent tree would report every data block as leaked.
       */
      bt_u32 nritems = bitter_leaf_nritems(block);
      for (bt_u32 i = 0; i < nritems; i++) {
        struct bitter_item* item = bitter_leaf_item(block, i);
        struct bitter_key_cpu k;
        bitter_key_from_disk(&k, &item->key);

        if (k.type != BITTER_EXTENT_DATA) {
          continue;
        }

        if (bt_get_le32(&item->size) < BITTER_EXTENT_DATA_SIZE) {
          printf("  block %#llx slot %u: extent data item is %u bytes,"
                 " under the %d-byte header\n",
                 (unsigned long long)it.bytenr, i,
                 bt_get_le32(&item->size), BITTER_EXTENT_DATA_SIZE);
          problems++;
          continue;
        }

        {
          const struct bitter_extent_data* ed = bitter_leaf_data(block, i);
          bt_u64 db;

          /* An INLINE extent's bytes are inside this item, so it references no
           * separate range and there is nothing to count. */
          if (ed->type != BITTER_FILE_EXTENT_REG) {
            continue;
          }

          /* disk_bytenr 0 is the conventional spelling of a hole: a range with
           * an item but no backing extent.  It points at nothing, and 0 is the
           * reserved region anyway. */
          db = bt_get_le64(&ed->disk_bytenr);
          if (db == 0) {
            continue;
          }

          s = fsck_note_data_ref(seen, db);
          if (s < 0) {
            env->ops->put_block(env, buf);
            free(wl.item);
            return s;
          }
        }
      }
    }

    env->ops->put_block(env, buf);
  }

  free(wl.item);
  return problems;
}

static int fsck_check_header(const struct fsck_pending* p,
        const struct bitter_root* root, void* block) {

  const struct bitter_header* hdr = block;
  int problems = 0;

  /* Where the pointer came from, for every message below.  A root's pointer
   * lives outside the tree -- in the superblock or in a root item -- which is
   * what parent == 0 means. */
  char where[96];
  if (p->parent == 0) {
    snprintf(where, sizeof where, "the root of tree %llu",
             (unsigned long long)root->objectid);
  } else {
    snprintf(where, sizeof where, "block %#llx slot %u",
             (unsigned long long)p->parent, p->slot);
  }

  /* The parent said what tier this child sits at.  btree_search makes the same
   * check, but only on the one path a search happens to take. */
  if (hdr->level != p->level) {
    printf("  block %#llx: level %u, but %s says %u\n",
           (unsigned long long)p->bytenr, hdr->level, where, p->level);
    problems++;
  }

  /* The key_ptr duplicates the child's generation exactly so a stale pointer
   * is detectable without reading the child.  Here the child has been read, so
   * the duplicate can be confirmed. */
  bt_u64 gen = bt_get_le64(&hdr->generation);
  if (gen != p->generation) {
    printf("  block %#llx: generation %llu, but %s says %llu\n",
           (unsigned long long)p->bytenr, (unsigned long long)gen,
           where, (unsigned long long)p->generation);
    problems++;
  }

  /* The one field nothing else in the filesystem reads.  A mismatch is a block
   * cross-linked into two trees -- intact, correctly addressed, and belonging
   * to somebody else, which csum and bytenr both miss by design. */
  bt_u64 owner = bt_get_le64(&hdr->owner);
  if (owner != root->objectid) {
    printf("  block %#llx: owned by tree %llu, reached from tree %llu\n",
           (unsigned long long)p->bytenr, (unsigned long long)owner,
           (unsigned long long)root->objectid);
    problems++;
  }

  /* Structural.  Both checkers return a bitmask rather than a first error, so
   * one call reports every way the block is malformed -- decode it, because
   * "corrupt" is not something anyone can act on. */
  if (hdr->level == 0) {
    bt_u32 f = bitter_leaf_check(block);
    if (f) {
      printf("  leaf %#llx: ", (unsigned long long)p->bytenr);
      if (f & BITTER_LEAF_ERR_NRITEMS) printf("[more items than fit] ");
      if (f & BITTER_LEAF_ERR_BOUNDS)  printf("[payload outside the block] ");
      if (f & BITTER_LEAF_ERR_OVERLAP) printf("[payloads overlap] ");
      if (f & BITTER_LEAF_ERR_ANCHOR)  printf("[slot 0 not flush with the end] ");
      if (f & BITTER_LEAF_ERR_GAP)     printf("[free space not contiguous] ");
      if (f & BITTER_LEAF_ERR_ORDER)   printf("[keys out of order] ");
      putchar('\n');
      problems++;
    }
  } else {
    bt_u32 f = bitter_node_check(block);
    if (f) {
      printf("  node %#llx: ", (unsigned long long)p->bytenr);
      if (f & BITTER_NODE_ERR_NRITEMS)  printf("[more children than fit] ");
      if (f & BITTER_NODE_ERR_EMPTY)    printf("[no children] ");
      if (f & BITTER_NODE_ERR_BLOCKPTR) printf("[impossible child address] ");
      if (f & BITTER_NODE_ERR_ORDER)    printf("[keys out of order] ");
      putchar('\n');
      problems++;
    }
  }

  return problems;
}


/* ------------------------------------------------------------------ */
/* Comparison, and the program                                         */
/* ------------------------------------------------------------------ */

static int by_bytenr(const void* a, const void* b) {
  bt_u64 x = *(const bt_u64*)a, y = *(const bt_u64*)b;
  return (x > y) - (x < y);
}

/*
 * Walks the extent tree's items in address order alongside the sorted
 * observations, and reports every way the two disagree.
 *
 * A merge of two ordered sequences rather than a lookup per item: extent keys
 * sort by address and `seen` has just been sorted, so one pass over each finds
 * everything.  Returns the number of problems found, or negative on an error
 * that stopped the check.
 */
static long fsck_compare(struct bitter_fs_info* fs, struct fsck_refs* seen) {
  struct bitter_env* env = fs->env;
  long problems = 0;
  bt_size i = 0;                       /* cursor into the sorted observations */

  struct bitter_key_cpu key = { 0, 0, 0 };
  struct bitter_path path;
  bitter_path_init(&path);

  int s = btree_search(env, &fs->extent_root, &key, &path, 0, 0, 0);
  if (s < 0) {
    return s;
  }

  for (;;) {
    void*  leaf    = path.nodes[0]->b_data;
    bt_u32 nritems = bitter_leaf_nritems(leaf);

    for (bt_u32 n = 0; n < nritems; n++) {
      struct bitter_key_cpu k;
      bitter_key_from_disk(&k, &bitter_leaf_item(leaf, n)->key);

      if (bitter_item_get_size(bitter_leaf_item(leaf, n)) != BITTER_EXTENT_ITEM_SIZE) {
        printf("  extent 0x%llx: item is %u bytes, expected %d\n",
               (unsigned long long)k.objectid,
               bitter_item_get_size(bitter_leaf_item(leaf, n)),
               BITTER_EXTENT_ITEM_SIZE);
        problems++;
        continue;
      }

      struct bitter_extent_item* it = bitter_leaf_data(leaf, n);
      bt_u64 stored = bt_get_le64(&it->refs);
      bt_u64 flags  = bt_get_le64(&it->flags);

      /*
       * Everything observed BELOW this item's address is pointed at by
       * something and has no extent item at all -- the dangerous direction,
       * since the allocator believes those blocks are free.
       */
      while (i < seen->count && seen->bytenr[i] < k.objectid) {
        bt_u64 orphan = seen->bytenr[i];
        bt_u64 count  = 0;
        while (i < seen->count && seen->bytenr[i] == orphan) { count++; i++; }
        printf("  block 0x%llx: referenced %llu time(s), no extent item\n",
               (unsigned long long)orphan, (unsigned long long)count);
        problems++;
      }

      bt_u64 observed = 0;
      while (i < seen->count && seen->bytenr[i] == k.objectid) { observed++; i++; }

      if (flags & BITTER_EXTENT_FLAG_RESERVED) {
        /* Reachable from no tree by definition; "nothing points at it" is the
         * expected state rather than a leak. */
        if (observed != 0) {
          printf("  extent 0x%llx: reserved, but referenced %llu time(s)\n",
                 (unsigned long long)k.objectid, (unsigned long long)observed);
          problems++;
        }
      } else if (observed != stored) {
        printf("  extent 0x%llx: refs %llu, %llu pointer(s) found%s\n",
               (unsigned long long)k.objectid, (unsigned long long)stored,
               (unsigned long long)observed,
               observed == 0 ? "  (leaked)" : "");
        problems++;
      }
    }

    int r = btree_next_leaf(env, &fs->extent_root, &path);
    if (r < 0) {
      return r;
    }
    if (r == 1) {
      break;                           /* released by next_leaf */
    }
  }

  /* Anything left over is past the last extent item: referenced, unrecorded. */
  while (i < seen->count) {
    bt_u64 orphan = seen->bytenr[i];
    bt_u64 count  = 0;
    while (i < seen->count && seen->bytenr[i] == orphan) { count++; i++; }
    printf("  block 0x%llx: referenced %llu time(s), no extent item\n",
           (unsigned long long)orphan, (unsigned long long)count);
    problems++;
  }

  return problems;
}

int main(int argc, char* argv[]) {
  set_progname(argv[0]);

  if (argc != 2) {
    die("usage: %s <image>", get_progname());
  }
  const char* path = argv[1];

  /* Read-only, and not merely as caution: user_env_put_block writes only what
   * was dirtied and fsck dirties nothing, so a checker that CANNOT write is
   * free.  It is also the point -- a tool run to find damage must not be able
   * to overwrite the evidence. */
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    die_errno("open %s", path);
  }

  struct env_user priv;
  struct bitter_env env;
  bitter_env_user_init(&env, &priv, fd, path);

  /*
   * Mounting builds the free-space map, which fsck never uses -- but it is
   * how bitter_fs_info_init wires the roots, and a checker is the worst place
   * to keep a second copy of the mount sequence.
   *
   * The retry is what -BITTER_ENOMEM was kept distinct from -BITTER_ENOSPC
   * for: it means the array was too small, not that the device is full.  A
   * badly fragmented filesystem is exactly the kind fsck gets run on, and a
   * fixed array would refuse the images most in need of checking.
   */
  struct bitter_fs_info fs;
  struct bitter_free_extent* free_arr = NULL;
  bt_u32 free_cap = 1024;
  int r;
  for (;;) {
    free(free_arr);
    free_arr = malloc(free_cap * sizeof *free_arr);
    if (!free_arr) {
      die("out of memory for %u free extents", free_cap);
    }
    memset(&fs, 0, sizeof fs);
    r = bitter_fs_info_init(&fs, &env, free_arr, free_cap);
    if (r != -BITTER_ENOMEM) {
      break;
    }
    if (free_cap > (bt_u32)(fs.total_bytes / BITTER_BLOCK_SIZE)) {
      die("free map will not fit even at %u entries", free_cap);
    }
    free_cap *= 2;
  }
  if (r < 0) {
    die("%s: cannot mount: %d", path, r);
  }

  print_section("filesystem");
  print_hex64("root tree",   fs.tree_root.bytenr);
  print_hex64("extent tree", fs.extent_root.bytenr);
  print_size ("total bytes", fs.total_bytes);
  print_u64  ("generation",  fs.tree_root.generation);

  /*
   * ONE observation set for every tree.  A block's refcount is the number of
   * pointers at it from anywhere in the filesystem, so counting per tree would
   * let a block referenced once by each look correct in both and wrong in
   * neither.
   */
  struct fsck_refs seen;
  memset(&seen, 0, sizeof seen);

  printf("\ntrees\n-----\n");
  long problems = fsck_observe_tree(&env, &fs.tree_root, &seen);
  if (problems < 0) {
    die("walking the root tree: %ld", problems);
  }
  long p2 = fsck_observe_tree(&env, &fs.extent_root, &seen);
  if (p2 < 0) {
    die("walking the extent tree: %ld", p2);
  }
  problems += p2;

  /*
   * The FS tree, found the same way the kernel's fill_super finds it.
   *
   * Not optional, and not merely a completeness nicety: every block of every
   * tree has an extent item, so a tree fsck does not walk contributes zero
   * pointers and its blocks all report as leaked.  Adding the FS tree to mkfs
   * without adding it here produced exactly that -- one spurious "leaked" on
   * the first image built.
   *
   * A root item bitterfs is expected to have and does not is itself a
   * finding, so this is an error rather than a skip.
   */
  struct bitter_root fs_root;
  memset(&fs_root, 0, sizeof fs_root);
  int fr = bitter_find_root(&env, &fs.tree_root, BITTER_FS_TREE_OBJECTID,
                            &fs_root);
  if (fr < 0) {
    die("looking up the FS tree: %d", fr);
  }
  fs_root.fs_info = &fs;

  long p3 = fsck_observe_tree(&env, &fs_root, &seen);
  if (p3 < 0) {
    die("walking the FS tree: %ld", p3);
  }
  problems += p3;

  /* Sorted so the comparison is a merge: the extent tree is already in address
   * order, being keyed by address. */
  qsort(seen.bytenr, seen.count, sizeof *seen.bytenr, by_bytenr);

  printf("\nextent tree\n-----------\n");
  long cmp = fsck_compare(&fs, &seen);
  if (cmp < 0) {
    die("checking the extent tree: %ld", cmp);
  }
  problems += cmp;

  printf("\n%ld problem(s) found\n", problems);

  free(seen.bytenr);
  free(free_arr);
  close(fd);

  /* e2fsck's convention, and what a script will test: 0 clean, 4 for errors
   * left uncorrected.  fsck cannot correct anything yet, so there is no 1. */
  return problems ? 4 : 0;
}
