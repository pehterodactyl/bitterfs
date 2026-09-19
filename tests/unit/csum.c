#include "test.h"
#include <stdlib.h>
#include "core/bitter_crc32c.h"
/*
 * The self-identifying checks in user_env_read_block: a block that did not
 * survive the trip must stop at the seam, and core/ must see it as -BITTER_EIO.
 *
 * Two separate mechanisms, so two separate corruptions.  The bytenr case
 * restamps the checksum deliberately -- otherwise the csum check fires first
 * and the bytenr comparison is never reached.
 */
#define N  50
#define SZ 500

static void ins(struct bt_fixture *f, struct bitter_path *p, bt_u64 oid)
{
        static unsigned char pay[SZ];
        struct bitter_key_cpu k = bt_key(oid);
        int r = btree_insert(&f->env, &f->root, &k, p, pay, SZ, &f->trans);
        CHECK_MSG(r == 0, "insert %llu -> %d", (unsigned long long)oid, r);
}

/* A plain lookup of a key that is present: 0 when the tree is readable, and
 * whatever error the seam raised when it is not. */
static int look(struct bt_fixture *f, bt_u64 oid)
{
        struct bitter_path q; bitter_path_init(&q);
        struct bitter_key_cpu k = bt_key(oid);
        int r = btree_search(&f->env, &f->root, &k, &q, 0, 0, 0);
        bitter_path_release(&f->env, &q);
        return r;
}

/* Raw, deliberately NOT wr(): wr() restamps the checksum, which is exactly
 * what this test needs not to happen. */
static void raw_read(struct bt_fixture *f, bt_u64 off, void *blk)
{
        if (pread(f->fd, blk, BITTER_BLOCK_SIZE, (off_t)off) != BITTER_BLOCK_SIZE)
                CHECK_MSG(0, "pread at 0x%llx", (unsigned long long)off);
}
static void raw_write(struct bt_fixture *f, bt_u64 off, const void *blk)
{
        if (pwrite(f->fd, blk, BITTER_BLOCK_SIZE, (off_t)off) != BITTER_BLOCK_SIZE)
                CHECK_MSG(0, "pwrite at 0x%llx", (unsigned long long)off);
}

int main(void)
{
        TEST_BEGIN("csum");
        struct bt_fixture f;
        bt_fixture_open(&f, "csum", 16 << 20);
        bt_make_empty_tree(&f, 0x12000);
        bt_fixture_free_from(&f, 0x13000);
        struct bitter_path p; bitter_path_init(&p);

        for (int i = 0; i < N; i++)
                ins(&f, &p, (bt_u64)i + 1);
        bitter_path_release(&f.env, &p);
        CHECK_EQ(bt_check_tree(&f), 0);

        /* A leaf as well as the root, so both bitter_leaf_check's block type and
         * an interior one are covered. */
        struct bitter_path q; bitter_path_init(&q);
        struct bitter_key_cpu k1 = bt_key(1);
        CHECK_EQ(btree_search(&f.env, &f.root, &k1, &q, 0, 0, 0), 0);
        bt_u64 leaf = q.nodes[0]->b_bytenr;
        bitter_path_release(&f.env, &q);
        printf("  root 0x%llx level %u, leaf 0x%llx\n",
               (unsigned long long)f.root.bytenr, f.root.level,
               (unsigned long long)leaf);

        CHECK_MSG(look(&f, 1) == 0, "healthy tree not readable");
        printf("  (the read_block messages below are the test working)\n");
        fflush(stdout);   /* stderr is unbuffered; keep the two streams in order */

        unsigned char orig[BITTER_BLOCK_SIZE], tmp[BITTER_BLOCK_SIZE];

        /* --- a flipped byte, checksum left stale --- */
        bt_u64 victims[2] = { f.root.bytenr, leaf };
        for (int v = 0; v < 2; v++) {
                raw_read(&f, victims[v], orig);
                memcpy(tmp, orig, sizeof tmp);
                /* Well past the header, inside what the checksum covers and
                 * inside what the block actually uses. */
                tmp[200] ^= 0xFF;
                raw_write(&f, victims[v], tmp);

                CHECK_MSG(look(&f, 1) == -BITTER_EIO,
                          "corrupt block at 0x%llx was accepted",
                          (unsigned long long)victims[v]);

                raw_write(&f, victims[v], orig);
                CHECK_MSG(look(&f, 1) == 0,
                          "tree not readable again after restoring 0x%llx",
                          (unsigned long long)victims[v]);
        }

        /* --- right block, wrong address: csum valid, bytenr a lie --- */
        raw_read(&f, leaf, orig);
        memcpy(tmp, orig, sizeof tmp);
        bt_put_le64(&((struct bitter_header *)tmp)->bytenr, leaf + BITTER_BLOCK_SIZE);
        bt_put_le32(((struct bitter_header *)tmp)->csum,
                    bt_block_csum(tmp, BITTER_BLOCK_SIZE));
        raw_write(&f, leaf, tmp);

        CHECK_MSG(look(&f, 1) == -BITTER_EIO,
                  "block claiming the wrong address was accepted");

        raw_write(&f, leaf, orig);
        CHECK_MSG(look(&f, 1) == 0, "tree not readable after restore");

        /* The io_error flag is for failed WRITES; nothing above wrote through
         * the seam, so bt_fixture_close must not report one. */
        bt_fixture_close(&f);
        TEST_END();
}
