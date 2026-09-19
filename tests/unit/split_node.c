#include "test.h"
/* split a node that is NOT the root: a 3-level shape is overkill, so build a
   root node with one child node, then split the child */
int main(void)
{
        TEST_BEGIN("split_node");
        struct bt_fixture f;
        bt_fixture_open(&f, "sn", 4 << 20);

        unsigned char blk[BITTER_BLOCK_SIZE];
        /* eight leaves at 0x20000, 0x21000, ... keys 10,20,...,80 */
        bt_u64 keys[8], kids[8];
        for (int i = 0; i < 8; i++) {
                keys[i] = 10 * (bt_u64)(i + 1);
                kids[i] = 0x20000 + (bt_u64)i * BITTER_BLOCK_SIZE;
                bt_build_leaf(blk, kids[i], 2, keys[i], 1, 8, (unsigned char)(0xA0 + i));
                wr(f.fd, blk, kids[i]);
        }
        /* level-1 node holding all eight */
        bt_build_node(blk, 0x12000, 1, 8, keys, kids);
        wr(f.fd, blk, 0x12000);
        /* level-2 root with one child */
        bt_u64 rk[1] = { 10 }, rc[1] = { 0x12000 };
        bt_build_node(blk, 0x11000, 2, 1, rk, rc);
        wr(f.fd, blk, 0x11000);

        f.root.bytenr = 0x11000; f.root.level = 2;
        bt_fixture_free_from(&f, 0x30000);
        CHECK_EQ(bt_check_tree(&f), 0);

        /* descend to the level-1 node the way a search would */
        struct bitter_path p; bitter_path_init(&p);
        struct bitter_key_cpu k = bt_key(70);
        int r = btree_search(&f.env, &f.root, &k, &p, 0, 0, 0);
        CHECK_EQ(r, 0);
        printf("  before: node at level 1 has %u children, root has %u\n",
               bitter_leaf_nritems(p.nodes[1]->b_data),
               bitter_leaf_nritems(p.nodes[2]->b_data));

        r = btree_split_node(&f.env, &p, 1, &f.root, &f.trans);
        CHECK_EQ(r, 0);
        printf("  after:  node at level 1 has %u children, root has %u\n",
               bitter_leaf_nritems(p.nodes[1]->b_data),
               bitter_leaf_nritems(p.nodes[2]->b_data));
        printf("  path->slots[1]=%d slots[2]=%d\n", p.slots[1], p.slots[2]);
        bitter_path_release(&f.env, &p);

        printf("  whole-tree check after the split:\n");
        CHECK_EQ(bt_check_tree(&f), 0);

        /* every key must still be findable */
        for (int i = 0; i < 8; i++) {
                struct bitter_path q; bitter_path_init(&q);
                struct bitter_key_cpu kk = bt_key(10 * (bt_u64)(i + 1));
                int rr = btree_search(&f.env, &f.root, &kk, &q, 0, 0, 0);
                CHECK_MSG(rr == 0, "key %d lookup returned %d", 10 * (i + 1), rr);
                bitter_path_release(&f.env, &q);
        }
        bt_fixture_close(&f);
        TEST_END();
}
