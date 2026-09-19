#include "test.h"
/* build a leaf with explicit per-item sizes */
static void build_sizes(void *blk, const bt_u32 *sizes, bt_u32 n)
{
        unsigned char pay[BITTER_MAX_ITEM_SIZE];
        memset(blk, 0, BITTER_BLOCK_SIZE);
        memset(pay, 0xEE, sizeof pay);
        for (bt_u32 i = 0; i < n; i++) {
                struct bitter_key_cpu k = bt_key(10 * (bt_u64)(i + 1));
                bitter_leaf_insert(blk, &k, i, pay, sizes[i]);
        }
}
static void show(const char *what, const bt_u32 *sizes, bt_u32 n)
{
        unsigned char blk[BITTER_BLOCK_SIZE];
        build_sizes(blk, sizes, n);
        bt_u32 k = bitter_leaf_split_point(blk);
        bt_u32 used = BITTER_LEAF_DATA_SIZE - bitter_leaf_free_space(blk);
        /* cost of each half, to see whether it actually balances */
        bt_u32 left = 0;
        for (bt_u32 i = 0; i < k && i < n; i++)
                left += sizes[i] + (bt_u32)sizeof(struct bitter_item);
        printf("  %-26s n=%-3u used=%-5u k=%-3u left=%-5u right=%-5u %s\n",
               what, n, used, k, left, used - left,
               (k >= 1 && k <= n - 1) ? "" : "  <-- OUT OF RANGE");
        CHECK_MSG(k >= 1 && k <= n - 1, "%s: k=%u with n=%u", what, k, n);
}
int main(void)
{
        TEST_BEGIN("leaf split point");
        bt_u32 a[64];

        for (bt_u32 i = 0; i < 10; i++) a[i] = 100;
        show("10 x 100", a, 10);

        for (bt_u32 i = 0; i < 40; i++) a[i] = 8;
        show("40 x 8 (descriptors rule)", a, 40);

        a[0] = 10; a[1] = 100;
        show("skewed: 10, 100", a, 2);

        a[0] = 100; a[1] = 10;
        show("skewed: 100, 10", a, 2);

        a[0] = 3000; a[1] = 10; a[2] = 10;
        show("one huge, two tiny", a, 3);

        a[0] = 10; a[1] = 10; a[2] = 3000;
        show("two tiny, one huge", a, 3);

        a[0] = 1900; a[1] = 1900;
        show("two equal halves", a, 2);

        for (bt_u32 i = 0; i < 2; i++) a[i] = 8;
        show("minimum splittable (2)", a, 2);

        TEST_END();
}
