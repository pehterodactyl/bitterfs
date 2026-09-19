/* bitter_node_insert: the three insertion positions, capacity, and that the
   node stays valid throughout.  items.c in isolation -- no environment. */
#include "test.h"

static bt_u64 key_at(void *blk, bt_u32 slot)
{
        struct bitter_key_cpu k;
        bitter_key_from_disk(&k, &bitter_node_key_ptr(blk, slot)->key);
        return k.objectid;
}

int main(void)
{
        TEST_BEGIN("node insert");
        unsigned char blk[BITTER_BLOCK_SIZE];
        bt_u64 keys[4] = { 10, 20, 30, 40 };
        bt_u64 kids[4] = { 0x12000, 0x13000, 0x14000, 0x15000 };

        /* middle */
        bt_build_node(blk, 0, 1, 4, keys, kids);
        { struct bitter_key_cpu k = bt_key(25);
          bitter_node_insert(blk, 2, &k, 0x16000, 1); }
        CHECK_NODE(blk);
        CHECK_EQ(bitter_leaf_nritems(blk), 5);
        { bt_u64 want[5] = {10,20,25,30,40};
          for (bt_u32 i = 0; i < 5; i++)
                CHECK_MSG(key_at(blk,i)==want[i], "slot %u = %"PRIu64, i, key_at(blk,i)); }
        CHECK_EQ(bitter_node_blockptr(blk, 2), 0x16000);
        CHECK_EQ(bitter_node_generation(blk, 2), 1);
        /* the entries that moved kept their own pointers */
        CHECK_EQ(bitter_node_blockptr(blk, 3), 0x14000);
        CHECK_EQ(bitter_node_blockptr(blk, 4), 0x15000);

        /* front -- every existing entry moves */
        bt_build_node(blk, 0, 1, 4, keys, kids);
        { struct bitter_key_cpu k = bt_key(5);
          bitter_node_insert(blk, 0, &k, 0x16000, 1); }
        CHECK_NODE(blk);
        CHECK_EQ(bitter_leaf_nritems(blk), 5);
        { bt_u64 want[5] = {5,10,20,30,40};
          for (bt_u32 i = 0; i < 5; i++)
                CHECK_MSG(key_at(blk,i)==want[i], "slot %u = %"PRIu64, i, key_at(blk,i)); }
        CHECK_EQ(bitter_node_blockptr(blk, 4), 0x15000);

        /* append -- slot == nritems, nothing moves */
        bt_build_node(blk, 0, 1, 4, keys, kids);
        { struct bitter_key_cpu k = bt_key(50);
          bitter_node_insert(blk, 4, &k, 0x16000, 1); }
        CHECK_NODE(blk);
        CHECK_EQ(bitter_leaf_nritems(blk), 5);
        CHECK_EQ(key_at(blk, 4), 50);

        /* into an empty node */
        bt_build_node(blk, 0, 1, 0, keys, kids);
        { struct bitter_key_cpu k = bt_key(7);
          bitter_node_insert(blk, 0, &k, 0x16000, 1); }
        CHECK_NODE(blk);
        CHECK_EQ(bitter_leaf_nritems(blk), 1);
        CHECK_EQ(key_at(blk, 0), 7);

        /* fill to capacity, always at the front, so every entry moves each time */
        bt_build_node(blk, 0, 1, 0, keys, kids);
        for (bt_u32 i = 0; i < BITTER_MAX_CHILDREN; i++) {
                struct bitter_key_cpu k = bt_key(BITTER_MAX_CHILDREN - i);
                bitter_node_insert(blk, 0, &k, 0x12000 + (bt_u64)i * BITTER_BLOCK_SIZE, 1);
                CHECK_MSG(bitter_node_check(blk) == 0, "after %u inserts", i + 1);
        }
        CHECK_EQ(bitter_leaf_nritems(blk), BITTER_MAX_CHILDREN);
        CHECK_EQ(bitter_node_free_slots(blk), 0);
        for (bt_u32 i = 0; i < BITTER_MAX_CHILDREN; i++)
                CHECK_MSG(key_at(blk, i) == i + 1, "full node slot %u", i);

        TEST_END();
}
