#include "test.h"
#include "trans.h"
#include <stdlib.h>
#include <fcntl.h>
/*
 * The commit ordering, which is the one property in phase 3 that no ordinary
 * test can reach: at every instant during a commit, the image on disk must be
 * EITHER the old filesystem or the new one.  Never a mixture.
 *
 * A power cut is simulated by copying the image mid-protocol.  env_user pwrites
 * on release, so the copy holds exactly the writes issued so far -- which is
 * what a machine that lost power at that moment would have.
 *
 * The interesting crash point is between "the new tree's blocks are on disk"
 * and "the superblock names them".  Everything the new tree needs is present in
 * that window, and the filesystem must still come up as the OLD one, because
 * nothing points at any of it.  That window is the entire reason the
 * superblock is written last.
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

/* The crash: everything written so far, nothing after. */
static void snapshot(const char *src, const char *dst)
{
        int in = open(src, O_RDONLY), out = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK_MSG(in >= 0 && out >= 0, "snapshot open");
        char buf[64 << 10];
        ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0)
                CHECK_MSG(write(out, buf, (size_t)n) == n, "snapshot write");
        close(in); close(out);
}

/* Open a snapshot as a fixture, without formatting it: the image already is a
 * filesystem, and read_super is what reconstructs the handle. */
static int mount_snapshot(struct bt_fixture *f, const char *path)
{
        snprintf(f->path, sizeof f->path, "%s", path);
        f->fd = open(path, O_RDWR);
        CHECK_MSG(f->fd >= 0, "open snapshot %s", path);
        bitter_env_user_init(&f->env, &f->priv, f->fd, f->path);
        int r = bitter_read_super(&f->env, &f->root);
        if (r) {
                return r;
        }

        /* This path skips bt_fixture_open, so nothing has wired the fixture's
         * filesystem yet: the retry below inserts, which allocates, which
         * dereferences root.fs_info.  Free space starts at the high-water mark
         * the superblock recorded -- everything below it is the tree this
         * snapshot is meant to preserve. */
        /* Zeroed like bt_fixture_open does: trans_commit reads
         * extent_root.bytenr to decide whether there is an extent tree, and
         * this path builds the fixture by hand. */
        memset(&f->fs, 0, sizeof f->fs);
        f->fs.env         = &f->env;
        f->fs.total_bytes = f->root.total_bytes;
        f->fs.free        = f->free_arr;
        f->fs.free_cap    = BT_FIXTURE_FREE_CAP;
        f->fs.free_count  = 0;
        f->root.fs_info   = &f->fs;
        bt_fixture_free_from(f, f->root.next_free);

        /* And somewhere to record the allocations the retry makes. */
        f->trans.refs      = f->ref_arr;
        f->trans.ref_cap   = BT_FIXTURE_REF_CAP;
        f->trans.ref_count = 0;

        return 0;
}

static bt_u32 keys_in(struct bt_fixture *f, bt_u64 *out, bt_u32 max)
{
        return bt_collect_keys(f, out, max);
}

int main(void)
{
        TEST_BEGIN("crash");
        char snap_pre[64], snap_post[64];
        snprintf(snap_pre,  sizeof snap_pre,  "/tmp/bt_crash_pre.img");
        snprintf(snap_post, sizeof snap_post, "/tmp/bt_crash_post.img");

        struct bt_fixture f;
        bt_fixture_mkfs(&f, "crash", 64 << 20);
        bt_u64 orig_root = f.root.bytenr;

        struct bitter_path p; bitter_path_init(&p);
        for (int i = 0; i < N; i++)
                ins(&f, &p, (bt_u64)i + 1);
        bitter_path_release(&f.env, &p);

        /* Every block of the new tree is on disk now.  Nothing points at it. */
        snapshot(f.path, snap_pre);

        int c = trans_commit(&f.fs, &f.trans, &f.root);
        CHECK_MSG(c == 0, "trans_commit -> %d", c);
        snapshot(f.path, snap_post);
        bt_u64 new_root = f.root.bytenr;
        bt_fixture_close(&f);

        bt_u64 *got = malloc(sizeof(bt_u64) * (N + 16));

        /* --- crash BEFORE the superblock write --- */
        {
                struct bt_fixture a;
                CHECK_EQ(mount_snapshot(&a, snap_pre), 0);
                CHECK_MSG(a.root.generation == 1, "generation %llu, expected 1",
                          (unsigned long long)a.root.generation);
                CHECK_MSG(a.root.bytenr == orig_root,
                          "root 0x%llx, expected the pre-commit 0x%llx",
                          (unsigned long long)a.root.bytenr,
                          (unsigned long long)orig_root);
                CHECK_EQ(bt_check_tree(&a), 0);
                CHECK_EQ((int)keys_in(&a, got, N + 16), 0);
                printf("  crash before the super write: old filesystem, 0 keys, intact\n");

                /* And the abandoned blocks are genuinely free: a new transaction
                 * reuses that space and commits cleanly. */
                /* A transaction built by hand needs its delayed-ref array
                 * too: trans_start only sets the generation, and every
                 * allocation records a ref. */
                struct bitter_trans t;
                memset(&t, 0, sizeof t);
                t.refs    = a.ref_arr;
                t.ref_cap = BT_FIXTURE_REF_CAP;
                CHECK_EQ(trans_start(&a.env, &t), 0);
                CHECK_MSG(t.generation == 2, "retry generation %llu, expected 2",
                          (unsigned long long)t.generation);
                struct bitter_path q; bitter_path_init(&q);
                static const unsigned char pay[8] = { 0 };
                for (int i = 0; i < 50; i++) {
                        struct bitter_key_cpu k = bt_key((bt_u64)i + 1000);
                        int r = btree_insert(&a.env, &a.root, &k, &q, pay, sizeof pay, &t);
                        CHECK_MSG(r == 0, "retry insert %d -> %d", i + 1000, r);
                }
                bitter_path_release(&a.env, &q);
                CHECK_EQ(trans_commit(&a.fs, &t, &a.root), 0);
                CHECK_EQ(bt_check_tree(&a), 0);
                CHECK_EQ((int)keys_in(&a, got, N + 16), 50);
                printf("  the retry after that crash commits cleanly\n");
                close(a.fd);
        }

        /* --- crash AFTER the superblock write --- */
        {
                struct bt_fixture b;
                CHECK_EQ(mount_snapshot(&b, snap_post), 0);
                CHECK_MSG(b.root.generation == 2, "generation %llu, expected 2",
                          (unsigned long long)b.root.generation);
                CHECK_MSG(b.root.bytenr == new_root, "root not the committed one");
                CHECK_EQ(bt_check_tree(&b), 0);
                bt_u32 n = keys_in(&b, got, N + 16);
                CHECK_MSG(n == N, "collected %u, expected %d", n, N);
                printf("  crash after the super write: new filesystem, %u keys, intact\n", n);
                close(b.fd);
        }

        free(got);
        unlink(snap_pre); unlink(snap_post);
        TEST_END();
}
