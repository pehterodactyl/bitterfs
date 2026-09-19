#include "test.h"
#include <stdlib.h>
/*
 * btree_cow_block: a tree built in one transaction, then modified in the next.
 *
 * The assertion that matters is about the OLD root.  After transaction 3 has
 * rewritten every block it touched, transaction 2's tree must still be walkable
 * and hold exactly the keys it held -- which is the whole premise of copy on
 * write, and, with no extra machinery, a snapshot.
 *
 * The suite before this one never reached the parent branch of
 * btree_cow_block: fixture blocks are generation 0 but the initial tree is a
 * single leaf that IS the root, and every block after it is born owned by the
 * current transaction.  Only a second transaction meets an older interior
 * block.
 */
#define N     1200
#define EXTRA 200
#define SZ    500

static void ins(struct bt_fixture *f, struct bitter_path *p, bt_u64 oid)
{
        static unsigned char pay[SZ];
        struct bitter_key_cpu k = bt_key(oid);
        int r = btree_insert(&f->env, &f->root, &k, p, pay, SZ, &f->trans);
        CHECK_MSG(r == 0, "insert %llu -> %d", (unsigned long long)oid, r);
}

/* The walkers take their root from the fixture, so reading a different tree
 * means pointing the fixture at it and putting it back. */
static bt_u32 collect_from(struct bt_fixture *f, bt_u64 bytenr, bt_u8 level,
                           bt_u64 *out, bt_u32 max, int *problems)
{
        bt_u64 save_b = f->root.bytenr;
        bt_u8  save_l = f->root.level;
        f->root.bytenr = bytenr;
        f->root.level  = level;
        *problems = bt_check_tree(f);
        bt_u32 n = bt_collect_keys(f, out, max);
        f->root.bytenr = save_b;
        f->root.level  = save_l;
        return n;
}

int main(void)
{
        TEST_BEGIN("cow");
        struct bt_fixture f;
        bt_fixture_open(&f, "cow", 64 << 20);
        bt_make_empty_tree(&f, 0x12000);
        bt_fixture_free_from(&f, 0x13000);
        struct bitter_path p; bitter_path_init(&p);

        /* ---- transaction 2: build the tree ---- */
        for (int i = 0; i < N; i++)
                ins(&f, &p, (bt_u64)(((long long)i * 7919) % N) + 1);
        CHECK_EQ(bt_check_tree(&f), 0);

        bt_u64 old_root  = f.root.bytenr;
        bt_u8  old_level = f.root.level;
        printf("  txn 2: %d keys, root 0x%llx level %u\n",
               N, (unsigned long long)old_root, old_level);

        /* Without an interior block below the root, the parent branch of
         * btree_cow_block still never runs and this test proves nothing. */
        CHECK_MSG(old_level >= 2, "root level %u -- need >= 2", old_level);

        /* ---- transaction 3 ---- */
        f.trans.generation = 3;

        /* Every block on this descent belongs to transaction 2, so all of them
         * relocate -- including the root. */
        ins(&f, &p, (bt_u64)N + 1);
        CHECK_MSG(f.root.bytenr != old_root,
                  "root did not move on the first write of a new transaction");
        bt_u64 txn3_root = f.root.bytenr;
        bt_u8  txn3_level = f.root.level;

        /* And the second must not move it again: those blocks are generation 3
         * now, so btree_cow_block owes its early return.  Skipped if the insert
         * happened to grow a new root, which moves it legitimately. */
        ins(&f, &p, (bt_u64)N + 2);
        if (f.root.level == txn3_level)
                CHECK_MSG(f.root.bytenr == txn3_root,
                          "root relocated twice inside one transaction");

        for (int i = 3; i <= EXTRA; i++)
                ins(&f, &p, (bt_u64)N + (bt_u64)i);
        printf("  txn 3: +%d keys, root 0x%llx level %u\n",
               EXTRA, (unsigned long long)f.root.bytenr, f.root.level);

        CHECK_MSG(bt_check_tree(&f) == 0, "transaction 3's tree is inconsistent");

        bt_u64 *got = malloc(sizeof(bt_u64) * (N + EXTRA + 16));
        bt_u32 n = bt_collect_keys(&f, got, N + EXTRA + 16);
        CHECK_MSG(n == N + EXTRA, "new tree has %u keys, expected %d",
                  n, N + EXTRA);
        for (bt_u32 i = 0; i < n; i++)
                CHECK_MSG(got[i] == (bt_u64)i + 1, "new key %u = %llu",
                          i, (unsigned long long)got[i]);

        /* ---- the property phase 3 exists for ---- */
        int problems = 0;
        bt_u32 m = collect_from(&f, old_root, old_level, got,
                                N + EXTRA + 16, &problems);
        CHECK_MSG(problems == 0, "transaction 2's tree no longer walks");
        CHECK_MSG(m == N, "old root holds %u keys, expected %d", m, N);
        for (bt_u32 i = 0; i < m && i < N; i++)
                CHECK_MSG(got[i] == (bt_u64)i + 1, "old key %u = %llu",
                          i, (unsigned long long)got[i]);
        printf("  txn 2's tree still holds its own %u keys\n", m);

        free(got);
        bitter_path_release(&f.env, &p);
        bt_fixture_close(&f);
        TEST_END();
}
