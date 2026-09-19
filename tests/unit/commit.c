#include "test.h"
#include "trans.h"
#include <stdlib.h>
#include <fcntl.h>
/*
 * trans_start / trans_commit over a real image, and the first test in the suite
 * that reopens a filesystem.  Everything before this one built a tree in a bare
 * file and read it back through the handle it had just been building -- which
 * cannot tell a committed filesystem from an uncommitted one.
 */
#define N  300
#define SZ 400

static void ins(struct bt_fixture *f, struct bitter_path *p, bt_u64 oid)
{
        static unsigned char pay[SZ];
        struct bitter_key_cpu k = bt_key(oid);
        int r = btree_insert(&f->env, &f->root, &k, p, pay, SZ, &f->trans);
        CHECK_MSG(r == 0, "insert %llu -> %d", (unsigned long long)oid, r);
}

int main(void)
{
        TEST_BEGIN("commit");
        struct bt_fixture f;
        bt_fixture_mkfs(&f, "commit", 64 << 20);

        /* Straight off the disk mkfs's formatters just wrote. */
        CHECK_EQ((int)f.root.bytenr, BITTER_SUPER_OFFSET + BITTER_BLOCK_SIZE);
        CHECK_EQ((int)f.root.level, 0);
        CHECK_EQ((int)f.root.generation, 1);
        CHECK_EQ((int)f.root.next_free, BITTER_SUPER_OFFSET + 2 * BITTER_BLOCK_SIZE);
        CHECK_MSG(f.trans.generation == 2, "trans generation %llu, expected 2",
                  (unsigned long long)f.trans.generation);

        bt_u64 old_root = f.root.bytenr;

        struct bitter_path p; bitter_path_init(&p);
        for (int i = 0; i < N; i++)
                ins(&f, &p, (bt_u64)i + 1);

        /* writeback_all's precondition: nothing still held. */
        bitter_path_release(&f.env, &p);

        CHECK_MSG(f.root.bytenr != old_root, "root did not move in txn 2");
        CHECK_EQ(bt_check_tree(&f), 0);

        int c = trans_commit(&f.fs, &f.trans, &f.root);
        CHECK_MSG(c == 0, "trans_commit -> %d", c);
        printf("  committed: root 0x%llx level %u next_free 0x%llx\n",
               (unsigned long long)f.root.bytenr, f.root.level,
               (unsigned long long)f.root.next_free);

        /* --- reopen: a second fd, a second env, nothing carried in memory --- */
        int fd2 = open(f.path, O_RDWR);
        CHECK_MSG(fd2 >= 0, "reopen %s", f.path);
        struct bitter_env e2; struct env_user pv2;
        bitter_env_user_init(&e2, &pv2, fd2, f.path);

        struct bitter_root r2;
        CHECK_EQ(bitter_read_super(&e2, &r2), 0);
        CHECK_MSG(r2.bytenr == f.root.bytenr, "root 0x%llx, committed 0x%llx",
                  (unsigned long long)r2.bytenr, (unsigned long long)f.root.bytenr);
        CHECK_EQ((int)r2.level, (int)f.root.level);
        CHECK_MSG(r2.generation == 2, "generation %llu, expected 2",
                  (unsigned long long)r2.generation);
        CHECK_MSG(r2.next_free == f.root.next_free, "next_free not persisted");

        int missing = 0;
        for (int i = 0; i < N; i++) {
                struct bitter_path q; bitter_path_init(&q);
                struct bitter_key_cpu k = bt_key((bt_u64)i + 1);
                if (btree_search(&e2, &r2, &k, &q, 0, 0, 0) != 0)
                        missing++;
                bitter_path_release(&e2, &q);
        }
        CHECK_MSG(missing == 0, "%d of %d keys unreadable after reopen", missing, N);
        printf("  reopened: %d keys readable from a tree this process never built\n",
               N - missing);

        /* A second transaction on the reopened filesystem starts at 3. */
        struct bitter_trans t3;
        CHECK_EQ(trans_start(&e2, &t3), 0);
        CHECK_MSG(t3.generation == 3, "second trans generation %llu, expected 3",
                  (unsigned long long)t3.generation);

        close(fd2);
        bt_fixture_close(&f);
        TEST_END();
}
