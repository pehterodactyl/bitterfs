#include "test.h"

/*
 * btree_split_leaf, all four shapes.
 *
 * Each case builds a full leaf, drives a real search down to it, splits, and
 * then finishes the insert the way btree_insert's tail does -- because a split
 * on its own proves very little.  The bugs this is aimed at (a wrong offset
 * delta on the moved descriptors, a separator left stale) only become visible
 * once the item is actually in and the whole tree is walked, so every case ends
 * in bt_check_tree plus a lookup of every key that should now exist.
 */

/*
 * Stands in for the fixup_low_keys call btree_insert makes after inserting at
 * slot 0.  That function is static in btree.c and these trees are two levels
 * deep, so the fixup is a single write into the parent -- no walk needed.
 */
static void fixup_parent(struct bitter_env *env, struct bitter_path *p,
                         const struct bitter_key_cpu *key)
{
        if (p->slots[0] != 0 || !p->nodes[1])
                return;
        struct bitter_key_ptr *kp =
                bitter_node_key_ptr(p->nodes[1]->b_data, (bt_u32)p->slots[1]);
        bitter_key_to_disk(&kp->key, key);
        env->ops->dirty_block(env, p->nodes[1]);
}

/* split at the position a search for `key` lands on, then insert there. */
static void split_and_insert(struct bt_fixture *f, bt_u64 objectid,
                             bt_u32 size, unsigned char fill, const char *what)
{
        struct bitter_key_cpu key = bt_key(objectid);
        unsigned char payload[BITTER_MAX_ITEM_SIZE];
        memset(payload, fill, size);

        struct bitter_path p;
        bitter_path_init(&p);
        int r = btree_search(&f->env, &f->root, &key, &p, 0, 0, 0);
        CHECK_MSG(r == 1, "%s: search returned %d, expected 1 (not found)", what, r);

        bt_u32 before = bitter_leaf_nritems(p.nodes[0]->b_data);
        int slot = p.slots[0];

        r = btree_split_leaf(&f->env, &p, &f->root, &key, size, &f->trans);
        CHECK_MSG(r == 0, "%s: split returned %d", what, r);

        printf("  %-9s leaf had %u items, slot %d -> landed in a leaf of %u at slot %d\n",
               what, before, slot,
               bitter_leaf_nritems(p.nodes[0]->b_data), p.slots[0]);

        /* Split only makes room; the caller still does the insert. */
        CHECK_MSG(bitter_leaf_free_space(p.nodes[0]->b_data)
                          >= size + sizeof(struct bitter_item),
                  "%s: split left no room for the item", what);

        bitter_leaf_insert(p.nodes[0]->b_data, &key, p.slots[0], payload, size);
        f->env.ops->dirty_block(&f->env, p.nodes[0]);
        fixup_parent(&f->env, &p, &key);
        bitter_path_release(&f->env, &p);
}

/* every key in `want` must be findable, and nothing else may have appeared. */
static void expect_keys(struct bt_fixture *f, const bt_u64 *want, bt_u32 n,
                        const char *what)
{
        bt_u64 got[256];
        bt_u32 have = bt_collect_keys(f, got, 256);
        CHECK_MSG(have == n, "%s: tree holds %u keys, expected %u", what, have, n);
        for (bt_u32 i = 0; i < n && i < have; i++)
                CHECK_MSG(got[i] == want[i],
                          "%s: key %u is %llu, expected %llu", what, i,
                          (unsigned long long)got[i], (unsigned long long)want[i]);

        for (bt_u32 i = 0; i < n; i++) {
                struct bitter_path q;
                bitter_path_init(&q);
                struct bitter_key_cpu k = bt_key(want[i]);
                int r = btree_search(&f->env, &f->root, &k, &q, 0, 0, 0);
                CHECK_MSG(r == 0, "%s: lookup of %llu returned %d", what,
                          (unsigned long long)want[i], r);
                bitter_path_release(&f->env, &q);
        }
        CHECK_MSG(bt_check_tree(f) == 0, "%s: tree check failed", what);
}

/* Builds a one-leaf tree whose root IS the leaf (root->level == 0). */
static void leaf_root(struct bt_fixture *f, bt_u32 n, bt_u64 first, bt_u64 step,
                      bt_u32 size, unsigned char fill)
{
        unsigned char blk[BITTER_BLOCK_SIZE];
        bt_build_leaf(blk, 0x20000, n, first, step, size, fill);
        wr(f->fd, blk, 0x20000);
        f->root.bytenr = 0x20000;
        f->root.level = 0;
        bt_fixture_free_from(f, 0x30000);
}

/* Builds a level-1 root over a single full leaf, so `parent` already exists. */
static void node_root(struct bt_fixture *f, bt_u32 n, bt_u64 first, bt_u64 step,
                      bt_u32 size, unsigned char fill)
{
        unsigned char blk[BITTER_BLOCK_SIZE];
        bt_build_leaf(blk, 0x20000, n, first, step, size, fill);
        wr(f->fd, blk, 0x20000);
        bt_u64 rk[1] = { first }, rc[1] = { 0x20000 };
        bt_build_node(blk, 0x11000, 1, 1, rk, rc);
        wr(f->fd, blk, 0x11000);
        f->root.bytenr = 0x11000;
        f->root.level = 1;
        bt_fixture_free_from(f, 0x30000);
}

int main(void)
{
        TEST_BEGIN("split_leaf");

        /*
         * MIDPOINT, on a leaf that is also the root -- so this doubles as the
         * root-growth case: a new level-1 root must appear AND must be given a
         * pointer back to the old root, which is the step whose absence leaves
         * every existing key unreachable.
         */
        {
                struct bt_fixture f;
                bt_fixture_open(&f, "sl_mid", 4 << 20);
                leaf_root(&f, 34, 100, 100, 90, 0xA5);   /* 34 * 115 = 3910 of 4000 */
                CHECK_EQ(bt_check_tree(&f), 0);
                CHECK_EQ(f.root.level, 0);

                split_and_insert(&f, 1050, 90, 0x5A, "midpoint");

                CHECK_MSG(f.root.level == 1, "root did not grow, level is %u",
                          f.root.level);

                /* 100..1000, then the new 1050, then 1100..3400 */
                bt_u64 want[35];
                for (bt_u32 i = 0; i < 10; i++)
                        want[i] = 100 + 100 * (bt_u64)i;
                want[10] = 1050;
                for (bt_u32 i = 10; i < 34; i++)
                        want[i + 1] = 100 + 100 * (bt_u64)i;
                expect_keys(&f, want, 35, "midpoint");
                bt_fixture_close(&f);
        }

        /*
         * APPEND.  One item of the largest legal size fills the leaf, so there
         * is no midpoint at all -- nritems is 1 and bitter_leaf_split_point's clamp would
         * hand back 0.  The new item sorts after it and gets a leaf of its own.
         */
        {
                struct bt_fixture f;
                bt_fixture_open(&f, "sl_app", 4 << 20);
                node_root(&f, 1, 100, 100, BITTER_MAX_ITEM_SIZE, 0xB1);
                CHECK_EQ(bt_check_tree(&f), 0);

                split_and_insert(&f, 200, 3000, 0x1B, "append");

                bt_u64 want[2] = { 100, 200 };
                expect_keys(&f, want, 2, "append");
                bt_fixture_close(&f);
        }

        /*
         * PREPEND.  Same full single-item leaf, but the new item sorts BEFORE
         * it, so the new leaf goes in ahead of the original and the parent's
         * first key changes -- the one shape where split owes a fixup of its
         * own rather than leaving it to the caller.
         */
        {
                struct bt_fixture f;
                bt_fixture_open(&f, "sl_pre", 4 << 20);
                node_root(&f, 1, 100, 100, BITTER_MAX_ITEM_SIZE, 0xC2);
                CHECK_EQ(bt_check_tree(&f), 0);

                split_and_insert(&f, 50, 3000, 0x2C, "prepend");

                bt_u64 want[2] = { 50, 100 };
                expect_keys(&f, want, 2, "prepend");
                bt_fixture_close(&f);
        }

        /*
         * MIDDLE.  Three chunky items; the new one sorts between the first and
         * second and fits in neither half, so it needs a block to itself and
         * the tail needs another -- the only shape that adds two entries to the
         * parent, and the only one where two allocations can fail apart.
         *
         * 3 * (1305 + 25) = 3990 of 4000, so the leaf is full.  bitter_leaf_split_point
         * lands on 2, slot is 1, and 1400 + 25 exceeds the 1340 free bytes on
         * the left -- which is what forces the fallback.
         */
        {
                struct bt_fixture f;
                bt_fixture_open(&f, "sl_midl", 4 << 20);
                node_root(&f, 3, 100, 100, 1305, 0xD3);
                CHECK_EQ(bt_check_tree(&f), 0);

                split_and_insert(&f, 150, 1400, 0x3D, "middle");

                bt_u64 want[4] = { 100, 150, 200, 300 };
                expect_keys(&f, want, 4, "middle");
                bt_fixture_close(&f);
        }

        TEST_END();
}
