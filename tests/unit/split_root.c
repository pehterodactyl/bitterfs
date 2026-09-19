#include "test.h"
int main(void)
{
        TEST_BEGIN("split_node: growing a new root");
        struct bt_fixture f;
        bt_fixture_open(&f, "sr", 4 << 20);

        unsigned char blk[BITTER_BLOCK_SIZE];
        bt_u64 keys[8], kids[8];
        for (int i = 0; i < 8; i++) {
                keys[i] = 10 * (bt_u64)(i + 1);
                kids[i] = 0x20000 + (bt_u64)i * BITTER_BLOCK_SIZE;
                bt_build_leaf(blk, kids[i], 2, keys[i], 1, 8, (unsigned char)(0xA0 + i));
                wr(f.fd, blk, kids[i]);
        }
        /* the level-1 node IS the root -- so splitting it must grow the tree */
        bt_build_node(blk, 0x11000, 1, 8, keys, kids);
        wr(f.fd, blk, 0x11000);
        f.root.bytenr = 0x11000; f.root.level = 1; bt_fixture_free_from(&f, 0x30000);
        CHECK_EQ(bt_check_tree(&f), 0);

        struct bitter_path p; bitter_path_init(&p);
        struct bitter_key_cpu k = bt_key(70);
        CHECK_EQ(btree_search(&f.env, &f.root, &k, &p, 0, 0, 0), 0);

        bt_u64 old_root = f.root.bytenr;
        CHECK_EQ(btree_split_node(&f.env, &p, 1, &f.root, &f.trans), 0);
        printf("  root moved %#" PRIx64 " -> %#" PRIx64 ", level 1 -> %u\n",
               old_root, f.root.bytenr, f.root.level);
        CHECK_EQ(f.root.level, 2);
        CHECK_MSG(f.root.bytenr != old_root, "root should have moved");
        bitter_path_release(&f.env, &p);

        CHECK_EQ(bt_check_tree(&f), 0);
        for (int i = 0; i < 8; i++) {
                struct bitter_path q; bitter_path_init(&q);
                struct bitter_key_cpu kk = bt_key(10 * (bt_u64)(i + 1));
                int rr = btree_search(&f.env, &f.root, &kk, &q, 0, 0, 0);
                CHECK_MSG(rr == 0, "key %d returned %d", 10 * (i + 1), rr);
                bitter_path_release(&f.env, &q);
        }
        bt_u64 got[16];
        bt_u32 n = bt_collect_keys(&f, got, 16);
        printf("  %u keys across all leaves:", n);
        for (bt_u32 i = 0; i < n; i++) printf(" %" PRIu64, got[i]);
        printf("\n");
        CHECK_EQ(n, 16);
        bt_fixture_close(&f);
        TEST_END();
}
