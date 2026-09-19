#include "test.h"
#include <stdlib.h>
/*
 * btree_del_item: deletion from slot 0 (fixup_low_keys on every call), the
 * not-found exit, draining the tree to empty, and refilling into the range the
 * emptied leaves left stale separators for.
 */
#define N 800
#define SZ 500
static void ins(struct bt_fixture *f, struct bitter_path *p, bt_u64 oid)
{
        static unsigned char pay[SZ];
        struct bitter_key_cpu k = bt_key(oid);
        int r = btree_insert(&f->env, &f->root, &k, p, pay, SZ, &f->trans);
        CHECK_MSG(r == 0, "insert %llu -> %d", (unsigned long long)oid, r);
}
static int del(struct bt_fixture *f, struct bitter_path *p, bt_u64 oid)
{
        struct bitter_key_cpu k = bt_key(oid);
        return btree_del_item(&f->env, &f->root, &k, p, &f->trans);
}
int main(void)
{
        TEST_BEGIN("del_item");
        struct bt_fixture f;
        bt_fixture_open(&f, "td", 64 << 20);
        bt_make_empty_tree(&f, 0x12000);
        bt_fixture_free_from(&f, 0x13000);
        struct bitter_path p; bitter_path_init(&p);

        for (int i = 0; i < N; i++)
                ins(&f, &p, (bt_u64)(((long long)i * 7919) % N) + 1);
        printf("  built: root level %u\n", f.root.level);
        CHECK_EQ(bt_check_tree(&f), 0);

        /* not-found: must be ENOENT and must not leak the path */
        CHECK_MSG(del(&f, &p, N + 500) == -BITTER_ENOENT, "missing key not ENOENT");
        {
                /* A path used for nothing else and deliberately NOT released
                   here: btree_del_item owes the release on the not-found exit,
                   and reusing the shared `p` would hide the omission, since the
                   next btree_search releases on entry. */
                struct bitter_path z; bitter_path_init(&z);
                struct bitter_key_cpu miss = bt_key(N + 501);
                CHECK_MSG(btree_del_item(&f.env, &f.root, &miss, &z, &f.trans)
                          == -BITTER_ENOENT, "missing key not ENOENT");
        }

        /* ascending deletion always removes slot 0 -- fixup_low_keys every time,
           and repeatedly walks past level 1 */
        for (int i = 1; i <= N / 2; i++) {
                int r = del(&f, &p, (bt_u64)i);
                CHECK_MSG(r == 0, "del %d -> %d", i, r);
                if (r) break;
        }
        CHECK_MSG(bt_check_tree(&f) == 0, "tree bad after slot-0 deletions");

        bt_u64 *got = malloc(sizeof(bt_u64) * (N + 16));
        bt_u32 n = bt_collect_keys(&f, got, N + 16);
        CHECK_MSG(n == N / 2, "collected %u, expected %d", n, N / 2);
        for (bt_u32 i = 0; i < n; i++)
                CHECK_MSG(got[i] == (bt_u64)(N / 2 + i + 1), "key %u = %llu",
                          i, (unsigned long long)got[i]);

        /* every survivor still findable, every corpse gone */
        for (int i = 1; i <= N; i++) {
                struct bitter_path q; bitter_path_init(&q);
                struct bitter_key_cpu k = bt_key((bt_u64)i);
                int r = btree_search(&f.env, &f.root, &k, &q, 0, 0, 0);
                CHECK_MSG(r == (i <= N / 2 ? 1 : 0), "lookup %d -> %d", i, r);
                bitter_path_release(&f.env, &q);
        }

        /* THE case that is correct for a non-obvious reason: delete a leaf to
           empty, then insert back into the range its stale separator covers. */
        for (int i = N / 2 + 1; i <= N; i++)
                CHECK_MSG(del(&f, &p, (bt_u64)i) == 0, "drain %d", i);
        printf("  drained to empty: root level %u\n", f.root.level);
        CHECK_EQ(bt_check_tree(&f), 0);
        CHECK_EQ((int)bt_collect_keys(&f, got, N + 16), 0);

        for (int i = 0; i < N; i++)
                ins(&f, &p, (bt_u64)(((long long)i * 6151) % N) + 1);
        CHECK_MSG(bt_check_tree(&f) == 0, "tree bad after refill");
        n = bt_collect_keys(&f, got, N + 16);
        CHECK_MSG(n == N, "refill collected %u, expected %d", n, N);
        for (bt_u32 i = 0; i < n; i++)
                CHECK_MSG(got[i] == (bt_u64)i + 1, "refill key %u = %llu",
                          i, (unsigned long long)got[i]);
        free(got);
        bitter_path_release(&f.env, &p);
        bt_fixture_close(&f);
        TEST_END();
}
