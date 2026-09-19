// SPDX-License-Identifier: GPL-2.0
/*
 * bitterfs — reading directories.
 *
 * A directory's entries are stored TWICE, under two key types with identical
 * payloads: (dir, DIR_ITEM, hash(name)) for lookup and (dir, DIR_INDEX, seq)
 * for readdir.  This file walks the second run; lookup searches the first.
 *
 * Keys sort (objectid, type, offset), so all of one directory's DIR_INDEX
 * items are contiguous -- which makes readdir a range scan rather than a
 * lookup per entry.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/string.h>

#include "format/bitterfs_format.h"
#include "core/bitter_endian.h"
#include "core/btree.h"
#include "core/items.h"
#include "core/bitter_assert.h"
#include "core/dir.h"
#include "kernel/bitterfs.h"

/*
 * BITTER_FT_* -> DT_*.
 *
 * Two numberings because the on-disk one must not be a Linux userspace ABI
 * value -- see the note on BITTER_FT_* in the format header.  This table is the
 * whole cost of that separation.
 */
static const unsigned char bitterfs_ft_to_dt[BITTER_FT_MAX] = {
	[BITTER_FT_UNKNOWN]  = DT_UNKNOWN,
	[BITTER_FT_REG_FILE] = DT_REG,
	[BITTER_FT_DIR]      = DT_DIR,
	[BITTER_FT_CHRDEV]   = DT_CHR,
	[BITTER_FT_BLKDEV]   = DT_BLK,
	[BITTER_FT_FIFO]     = DT_FIFO,
	[BITTER_FT_SOCK]     = DT_SOCK,
	[BITTER_FT_SYMLINK]  = DT_LNK,
};

/*
 * The type byte comes off the disk, so it is bounded before it indexes the
 * table.  DT_UNKNOWN is a legal answer that merely makes the caller stat the
 * target, so an unrecognised value degrades rather than failing the listing.
 */
static unsigned char bitterfs_dt(bt_u8 ft)
{
	if (ft >= BITTER_FT_MAX)
		return DT_UNKNOWN;
	return bitterfs_ft_to_dt[ft];
}

/*
 * The other direction: a VFS i_mode to the type byte a dirent stores.
 *
 * NOT the inverse of the table above, despite the symmetry.  That one is
 * indexed by BITTER_FT_* -- dense, 0..7 -- while the file-type bits of i_mode
 * are S_IFREG 0x8000, S_IFDIR 0x4000 and so on, sparse across sixteen bits and
 * unable to index an array at all.
 *
 * Shifting by 12 is the conventional fix (ext2 calls it S_SHIFT): it maps the
 * seven S_IFMT values onto 1, 2, 4, 6, 8, 10 and 12, so one small table covers
 * them with the gaps left at BITTER_FT_UNKNOWN, which is 0.
 *
 * Worth knowing about that shift, because it explains why this table is needed
 * at all: DT_* is DEFINED as (S_IFMT >> 12), so the shift on its own is already
 * the mode-to-DT conversion.  BITTER_FT_* follows ext2's dense numbering
 * instead -- REG_FILE 1, DIR 2, CHRDEV 3 -- which shares nothing with either.
 * Three numbering schemes for one concept, and this module touches all three.
 *
 * Sized S_IFMT's index plus one rather than exactly it: a mode with every type
 * bit set shifts to 15, which is one past ext2's array.  Unreachable from the
 * VFS, which is where mode comes from -- and a byte is cheaper than the
 * argument for why it cannot happen.
 */
#define BITTERFS_S_SHIFT 12

static const unsigned char
bitterfs_mode_to_ft[(S_IFMT >> BITTERFS_S_SHIFT) + 1] = {
	[S_IFREG  >> BITTERFS_S_SHIFT] = BITTER_FT_REG_FILE,
	[S_IFDIR  >> BITTERFS_S_SHIFT] = BITTER_FT_DIR,
	[S_IFCHR  >> BITTERFS_S_SHIFT] = BITTER_FT_CHRDEV,
	[S_IFBLK  >> BITTERFS_S_SHIFT] = BITTER_FT_BLKDEV,
	[S_IFIFO  >> BITTERFS_S_SHIFT] = BITTER_FT_FIFO,
	[S_IFSOCK >> BITTERFS_S_SHIFT] = BITTER_FT_SOCK,
	[S_IFLNK  >> BITTERFS_S_SHIFT] = BITTER_FT_SYMLINK,
};

/*
 * No bound needed, unlike bitterfs_dt: mode comes from the VFS rather than the
 * disk, and the mask plus the sizing above make every input in range.  An
 * undefined type yields BITTER_FT_UNKNOWN, which readdir turns back into
 * DT_UNKNOWN -- a legal answer that merely makes the caller stat the target.
 */
static unsigned char bitterfs_ft(umode_t mode)
{
	return bitterfs_mode_to_ft[(mode & S_IFMT) >> BITTERFS_S_SHIFT];
}

/*
 * Emit as many entries as fit in the caller's buffer.
 *
 * Called repeatedly -- getdents64 loops until a call produces nothing -- so the
 * resume path is the common case and not an edge case.  ctx->pos is the cursor,
 * loaded from file->f_pos before we run and stored back after; it IS a
 * DIR_INDEX key offset, which is what makes resumption a search rather than a
 * count.
 *
 * Returns 0 for everything normal, INCLUDING a full buffer and a finished
 * directory.  The caller cannot distinguish those and does not need to: it
 * calls again, and a call that emits nothing is how the directory ends.  A
 * negative errno is reserved for genuine failure.
 *
 * --- why the loop drops the lock every iteration --------------------------
 * dir_emit can fault: filldir64 writes to a user buffer, and if that buffer is
 * mmap'd from a file on THIS filesystem, faulting it in re-enters us.  Holding
 * mnt->lock across it is a self-deadlock.  So each iteration takes the lock,
 * extracts one entry into locals, drops the lock, and emits.
 *
 * btrfs batches instead -- collecting many entries under the lock and emitting
 * them after -- which is a real optimisation and more machinery.  See
 * docs/LOG.md.
 */
static int bitterfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode		*dir = file_inode(file);
	struct bitterfs_mount	*mnt = bitterfs_sb(dir->i_sb);
	bt_u64			 ino = dir->i_ino;

	struct bitter_path	 path;
	struct bitter_key_cpu	 key;
	struct bitter_key_cpu	 leaf_key;
	struct bitter_item	*it;
	struct bitter_dir_item	*di;
	struct bitter_buf	*node;

	bt_u8			 namebuf[BITTER_MAX_FILENAME];
	bt_u64			 target_ino;
	bt_u64			 index;
	bt_u16			 name_len;
	bt_u32			 size;
	bt_u32			 nritems;
	int			 slot;
	int			 s;
	int			 err;

	/*
	 * "." and ".." are synthesised from the dentry tree, not stored -- which
	 * is why the root directory has nlink 2 with no entries on disk.
	 *
	 * Also establishes the invariant the rest of this function needs: on
	 * return true, ctx->pos is >= 2 and is therefore a valid DIR_INDEX
	 * offset.  BITTER_DIR_START_INDEX is 2 for exactly this reason.
	 *
	 * false means the buffer could not fit even ".", which is a full buffer
	 * and not an error.
	 */
	if (!dir_emit_dots(file, ctx))
		return 0;

	/* Once: bitter_path_release clears the path, leaving it in the same
	 * state as after init, so it is reusable across iterations. */
	bitter_path_init(&path);

	for (;;) {
		bitter_dir_index_key(&key, ino, (bt_u64) ctx->pos);

		mutex_lock(&mnt->lock);

		/* ins_len 0 and trans NULL: a pure lookup that structurally
		 * cannot split or allocate. */
		s = btree_search(&mnt->env, &mnt->fs.fs_root, &key, &path, 0,
				 NULL, 0);
		if (s < 0) {
			err = s;
			goto out_unlock;
		}

		/*
		 * 0 and 1 are BOTH fine here, unlike in bitterfs_iget.  1 means
		 * the exact index is not present and slots[0] is where it would
		 * go -- i.e. the first entry AFTER the cursor, which is what a
		 * range scan wants.  Deleted indices leave gaps and the search
		 * steps over them for free.
		 */
		node    = path.nodes[0];
		slot    = path.slots[0];
		nritems = bitter_leaf_nritems(node->b_data);

		/*
		 * A 1 return can position slots[0] AT nritems -- past this
		 * leaf's last item -- and the next real entry is then in the
		 * next leaf.  btree_search does not cross leaves, so this is
		 * where the walk does.
		 *
		 * A while, not an if: btree_del_item leaves emptied leaves in
		 * the tree (see docs/LOG.md), so the next leaf can hold zero
		 * items and need skipping too.  Not reachable until something
		 * deletes, which is exactly why it would look correct.
		 */
		while ((bt_u32) slot >= nritems) {
			int n = btree_next_leaf(&mnt->env, &mnt->fs.fs_root,
						&path);
			if (n < 0) {
				/* The path is already released on a non-zero
				 * return -- btree_next_leaf says so -- but
				 * bitter_path_release is safe twice, so the
				 * label needs no special case. */
				err = n;
				goto out_unlock;
			}
			if (n > 0) {
				err = 0;       /* no more leaves: directory ends */
				goto out_unlock;
			}
			node    = path.nodes[0];
			slot    = path.slots[0];
			nritems = bitter_leaf_nritems(node->b_data);
		}

		it = bitter_leaf_item(node->b_data, (bt_u32) slot);
		bitter_key_from_disk(&leaf_key, &it->key);

		/*
		 * Both halves matter.  objectid says we are still on this
		 * directory; type says we are still among its INDEX entries.
		 * Any higher type under the same objectid follows immediately --
		 * the format header reserves room for XATTR_ITEM and others --
		 * and without the type check readdir would decode one of those
		 * as a dirent.
		 */
		if (leaf_key.objectid != ino ||
		    leaf_key.type != BITTER_DIR_INDEX) {
			err = 0;
			goto out_unlock;
		}

		/*
		 * Two steps, and the order is forced: name_len lives INSIDE the
		 * payload, so the item must be known large enough to hold a
		 * header before that read is legal.
		 */
		size = bt_get_le32(&it->size);
		if (size < BITTER_DIR_ITEM_HEADER_SIZE) {
			pr_err("dir %llu: entry item is %u bytes, under the %d-byte header\n",
			       (unsigned long long) ino, size,
			       BITTER_DIR_ITEM_HEADER_SIZE);
			err = -EUCLEAN;
			goto out_unlock;
		}

		di       = bitter_leaf_data(node->b_data, (bt_u32) slot);
		name_len = bt_get_le16(&di->name_len);

		/* A disk-supplied length: unbounded, this is a copy running off
		 * the end of a 4096-byte leaf. */
		if (name_len > BITTER_MAX_FILENAME ||
		    size != BITTER_DIR_ITEM_HEADER_SIZE + name_len) {
			pr_err("dir %llu: name_len %u does not match item size %u\n",
			       (unsigned long long) ino, name_len, size);
			err = -EUCLEAN;
			goto out_unlock;
		}

		/*
		 * Copied out because dir_emit runs outside the lock and di
		 * points into the leaf's buffer, which the release below drops.
		 *
		 * location.objectid is the target's inode number, cached here so
		 * that ls -i needs no second lookup.  Its .type is
		 * BITTER_INODE_ITEM for every entry today and is deliberately
		 * unchecked: a subvolume entry will name a ROOT_ITEM in the root
		 * tree instead, and that case has not arrived.
		 */
		memcpy(namebuf, di->name, name_len);
		target_ino = bt_get_le64(&di->location.objectid);
		index      = leaf_key.offset;
		s          = bitterfs_dt(di->type);

		bitter_path_release(&mnt->env, &path);
		mutex_unlock(&mnt->lock);

		/* --- lock NOT held below this line --------------------------- */

		/*
		 * pos must be THIS entry's index before the emit: dir_emit
		 * passes ctx->pos to the actor, and filldir64 stores it as the
		 * previous dirent's d_off -- the cookie userspace seeks to in
		 * order to resume past that entry.  Anything else and every
		 * d_off in the buffer is wrong, which breaks seekdir and NFS
		 * directory cookies.
		 */
		ctx->pos = (loff_t) index;

		if (!dir_emit(ctx, (const char *) namebuf, (int) name_len,
			      target_ino, (unsigned) s)) {
			/* Buffer full.  pos is left ON this entry so the next
			 * call retries it rather than skipping it.  Nothing is
			 * held here -- returning through out_unlock would
			 * unlock a mutex we do not own. */
			return 0;
		}

		/* One past what we just emitted -- not "the next entry's
		 * index", which may be far away.  The next search steps over
		 * any gap. */
		ctx->pos = (loff_t) index + 1;
	}

out_unlock:
	bitter_path_release(&mnt->env, &path);
	mutex_unlock(&mnt->lock);
	return err;
}

/*
 * Resolve one name in one directory.
 *
 * A cache-miss handler: the VFS calls this only when the dcache has no answer
 * for this name under this parent.  The dentry arrives already allocated with
 * the name filled in -- this function completes it, by attaching an inode or
 * leaving it negative.
 *
 * Returns NULL for success (including "no such name"), or an ERR_PTR.  A
 * NEGATIVE DENTRY IS SUCCESS: it caches the absence, so a repeated stat of a
 * missing file never reaches this code again.  Returning an error instead
 * would throw that away and, at phase 7, would make open(O_CREAT) fail rather
 * than proceed to create.
 */
static struct dentry *bitterfs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct bitterfs_mount	*mnt = bitterfs_sb(dir->i_sb);
	struct inode		*inode;
	struct bitter_path	 path;
	struct bitter_key_cpu	 key;
	struct bitter_item	*it;
	struct bitter_dir_item	*di;
	struct bitter_buf	*node;

	bt_u64			 name_hash;
	bt_u64			 target_ino;
	bt_u32			 size;
	bt_u16			 name_len;
	int			 slot;
	int			 s;
	int			 err;

	/* The VFS rejects anything longer than NAME_MAX before a filesystem
	 * method sees it, and BITTER_MAX_FILENAME is NAME_MAX -- so a length
	 * guard here would be unreachable. */

	/*
	 * Hashed straight out of the dentry: d_name.name is the VFS's and stays
	 * valid for this whole call, so no copy is needed.  readdir must copy
	 * because its source is a leaf buffer it then releases; this is not that
	 * situation.
	 *
	 * NOT dentry->d_name.hash -- that is the DCACHE's hash, a different
	 * function for a different table.  Using it would compile, produce a
	 * plausible number, and find nothing.
	 */
	name_hash = bitter_name_hash(dentry->d_name.name, dentry->d_name.len);
	bitter_dir_item_key(&key, dir->i_ino, name_hash);

	bitter_path_init(&path);

	mutex_lock(&mnt->lock);

	s = btree_search(&mnt->env, &mnt->fs.fs_root, &key, &path, 0, NULL, 0);
	if (s < 0) {
		err = s;
		goto out_err;
	}

	/*
	 * 1 means no entry hashes to this value -- the name does not exist.
	 * Success, not a failure: the third different meaning this return has
	 * across the three callers, because each asks a different question.
	 * bitterfs_iget treats 1 as corruption; readdir treats it as "here is
	 * the next entry".
	 */
	if (s != 0)
		goto out_notfound;

	/*
	 * s == 0 guarantees the key matched EXACTLY -- objectid and type
	 * included -- so unlike readdir there is no "is this still mine" check
	 * to make.  What it does not guarantee is the name.
	 */
	node = path.nodes[0];
	slot = path.slots[0];

	it   = bitter_leaf_item(node->b_data, (bt_u32) slot);
	size = bt_get_le32(&it->size);

	/* Two steps, and the order is forced: name_len lives inside the payload,
	 * so the item must be known big enough to hold a header first. */
	if (size < BITTER_DIR_ITEM_HEADER_SIZE) {
		pr_err("dir %llu: entry item is %u bytes, under the %d-byte header\n",
		       (unsigned long long) dir->i_ino, size,
		       BITTER_DIR_ITEM_HEADER_SIZE);
		err = -EUCLEAN;
		goto out_err;
	}

	di       = bitter_leaf_data(node->b_data, (bt_u32) slot);
	name_len = bt_get_le16(&di->name_len);

	if (name_len > BITTER_MAX_FILENAME ||
	    size != BITTER_DIR_ITEM_HEADER_SIZE + name_len) {
		pr_err("dir %llu: name_len %u does not match item size %u\n",
		       (unsigned long long) dir->i_ino, name_len, size);
		err = -EUCLEAN;
		goto out_err;
	}

	/*
	 * The KEY matched, which means the HASH matched.  Two different names
	 * can hash alike -- even odds somewhere around 65,000 entries -- so the
	 * stored name has to be compared.  This is the only reason the name is
	 * in the payload at all.
	 *
	 * A mismatch is NOT corruption: it is a collision, and the honest answer
	 * is that this name does not exist.  Which is currently also the true
	 * answer, because an item holds one entry and a colliding pair cannot
	 * both be stored.  When that changes, this branch becomes the start of a
	 * linear walk through the entries packed into this item rather than a
	 * dead end.  See docs/LOG.md.
	 *
	 * Length first: one comparison rules out most mismatches and is what
	 * makes the memcmp safe to bound at name_len.
	 */
	if (name_len != dentry->d_name.len ||
	    memcmp(di->name, dentry->d_name.name, name_len) != 0)
		goto out_notfound;

	/* location is a KEY.  Only objectid is read; .type is BITTER_INODE_ITEM
	 * for every entry today and is deliberately unchecked -- a subvolume
	 * entry will name a ROOT_ITEM in the root tree instead, and that branch
	 * belongs here when phase 8 needs it. */
	target_ino = bt_get_le64(&di->location.objectid);

	/* Path released, mutex KEPT: bitterfs_iget runs its own search with its
	 * own path, and it opens with lockdep_assert_held(&mnt->lock).
	 *
	 * This call is the one the lock move existed for.  Before it, lookup
	 * holding the mutex and calling a function that took it again was a
	 * self-deadlock on a non-recursive mutex. */
	bitter_path_release(&mnt->env, &path);

	inode = bitterfs_iget(dir->i_sb, target_ino);

	mutex_unlock(&mnt->lock);

	/*
	 * No IS_ERR check: d_splice_alias opens with `if (IS_ERR(inode)) return
	 * ERR_CAST(inode);`, so an error from iget becomes this function's
	 * return value untouched.  Outside the lock because it touches the
	 * dcache and can sleep.
	 */
	return d_splice_alias(inode, dentry);

out_notfound:
	/* A negative dentry, and a successful lookup. */
	bitter_path_release(&mnt->env, &path);
	mutex_unlock(&mnt->lock);
	return d_splice_alias(NULL, dentry);

out_err:
	bitter_path_release(&mnt->env, &path);
	mutex_unlock(&mnt->lock);
	return ERR_PTR(err);
}

/*
 * Hand back this directory's next DIR_INDEX and store the incremented value.
 *
 * Both halves in one visit, because the value read IS the index about to be
 * used and the increment is what stops the next create reusing it.  Splitting
 * them would mean two CoW searches of one item with a window in between.
 *
 * It exists at all because next_dir_index lives ONLY on disk: struct inode has
 * no field for it, bitterfs_iget reads it and drops it, and
 * bitterfs_update_inode deliberately preserves it -- which is precisely why
 * that function is a read-modify-write rather than a rebuild.
 *
 * A monotonic high-water mark, never a count.  Gaps are fine and expected: a
 * failure after this point leaves an index nothing uses, which costs nothing,
 * whereas REUSING one would let a resumed readdir miss an entry or repeat it,
 * since f_pos is this number.
 *
 * Caller holds mnt->lock and an open transaction.
 */
static int bitterfs_take_dir_index(struct bitter_trans *trans,
				   struct inode *dir, bt_u64 *out)
{
	struct bitterfs_mount	 *mnt = bitterfs_sb(dir->i_sb);
	struct bitter_env	 *env = &mnt->env;

	struct bitter_path	  path;
	struct bitter_key_cpu	  key;
	struct bitter_item	 *item;
	struct bitter_inode_item *disk;
	bt_u64			  index;
	int			  err;
	int			  s;

	lockdep_assert_held(&mnt->lock);

	key.objectid = dir->i_ino;
	key.type     = BITTER_INODE_ITEM;
	key.offset   = 0;

	bitter_path_init(&path);

	/* cow 1 and a live trans: this writes.  ins_len 0 -- the replacement is
	 * the same eight bytes under the same key, so nothing in the leaf
	 * moves. */
	s = btree_search(env, &mnt->fs.fs_root, &key, &path, 0, trans, 1);
	if (s != 0) {
		/* 1 means the parent's own inode item is missing, which is
		 * corruption rather than an insertion point. */
		err = (s < 0) ? s : -EUCLEAN;
		goto out;
	}

	/* Before any payload byte is read OR written -- a short item would put
	 * the eight bytes below into the neighbour's payload, and every leaf
	 * descriptor would still agree afterwards. */
	item = bitter_leaf_item(path.nodes[0]->b_data, (bt_u32) path.slots[0]);
	if (bt_get_le32(&item->size) != BITTER_INODE_ITEM_SIZE) {
		pr_err("dir %llu: inode item is %u bytes, expected %d\n",
		       (unsigned long long) dir->i_ino,
		       bt_get_le32(&item->size), BITTER_INODE_ITEM_SIZE);
		err = -EUCLEAN;
		goto out;
	}

	disk  = bitter_leaf_data(path.nodes[0]->b_data, (bt_u32) path.slots[0]);
	index = bt_get_le64(&disk->next_dir_index);

	/* Through the accessor both ways.  `disk->next_dir_index++` would do
	 * host arithmetic on little-endian bytes, and as a function argument it
	 * would also pass the value BEFORE the increment -- writing back exactly
	 * what was read. */
	bt_put_le64(&disk->next_dir_index, index + 1);

	/* Ours to dirty: nothing else knows the payload changed.  The CoW
	 * descent dirtied this leaf already, but that checksum covers the bytes
	 * as they were before this edit.  Before the release, which is brelse. */
	env->ops->dirty_block(env, path.nodes[0]);

	*out = index;
	err  = 0;

out:
	bitter_path_release(env, &path);
	return err;
}

/*
 * The backref: the other half of a name.
 *
 * add_dirent records that the DIRECTORY holds this name; this records that the
 * INODE answers to it.  Keyed (inode, INODE_REF, dir) -- objectid is the file
 * and offset is the directory, the exact reverse of a dirent's key, which is
 * what lets the pair be checked against each other.
 *
 * Nothing reads it yet.  It is written now because create is its only writer,
 * and a file created before the field existed would carry no backref forever --
 * so unlink would have to cope with two kinds of inode.
 *
 * `index` is the DIR_INDEX the entry was given.  Recorded here because that
 * value lives nowhere else once the entry exists, and unlink needs it to
 * delete the index entry without walking the directory comparing names.
 *
 * Caller holds mnt->lock and an open transaction.
 */
static int bitterfs_add_inode_ref(struct bitter_trans *trans, struct inode *dir,
				  struct inode *inode, const char *name,
				  int name_len, bt_u64 index)
{
	struct bitterfs_mount	 *mnt = bitterfs_sb(dir->i_sb);
	struct bitter_env	 *env = &mnt->env;

	struct bitter_path	  path;
	struct bitter_key_cpu	  key;
	struct bitter_inode_ref	 *ir;

	/* Header plus the longest name the format allows, same shape as the
	 * dirent buffer -- a flexible array member cannot be a local. */
	unsigned char		  buf[BITTER_INODE_REF_SIZE +
				      BITTER_MAX_FILENAME];
	bt_u32			  size;
	int			  s;

	lockdep_assert_held(&mnt->lock);
	BITTER_ASSERT(name_len > 0 && name_len <= BITTER_MAX_FILENAME);

	ir   = (struct bitter_inode_ref *) buf;
	size = BITTER_INODE_REF_SIZE + (bt_u32) name_len;

	bt_put_le64(&ir->index, index);
	bt_put_le16(&ir->name_len, (bt_u16) name_len);
	memcpy(ir->name, name, (size_t) name_len);

	key.objectid = inode->i_ino;
	key.type     = BITTER_INODE_REF;
	key.offset   = dir->i_ino;

	bitter_path_init(&path);

	s = btree_insert(env, &mnt->fs.fs_root, &key, &path, buf, size, trans);
	if (s == -BITTER_EEXIST) {
		/*
		 * This inode already has a backref in this directory, which
		 * means a second hard link to it here -- `ln a b` in one
		 * place.  The key is unique per (inode, parent), so both cannot
		 * exist as separate items.
		 *
		 * The same limitation bitter_dir_item has for name-hash
		 * collisions, with the same shape of fix: pack several into one
		 * item and walk them.  Nothing implements it, so this is
		 * well-formed and unsupported rather than corrupt.
		 *
		 * Unreachable from ->create, which is only ever called on a
		 * name lookup already proved absent.  It is ->link that will
		 * meet this.
		 */
		pr_err("inode %llu: already has a backref in dir %llu\n",
		       (unsigned long long) inode->i_ino,
		       (unsigned long long) dir->i_ino);
		return -EOPNOTSUPP;
	}

	/* btree_insert released the path on every exit. */
	return s;
}

/*
 * Attach one name to one inode: the DIR_ITEM and DIR_INDEX pair.
 *
 * Two items, one payload.  They hold byte-identical contents and differ only
 * in their key, which is the whole point of the pair -- the same entry indexed
 * two ways:
 *
 *   (dir, DIR_ITEM,  hash(name))   what lookup searches, keyed by NAME
 *   (dir, DIR_INDEX, index)        what readdir walks, keyed by CREATION ORDER
 *
 * So the buffer below is composed once and inserted twice.
 *
 * The payload is variable-length -- a 20-byte header and then the name -- so
 * it is built on the stack rather than being a struct: 275 bytes at the very
 * worst, which a kernel stack carries without complaint.
 *
 * Caller holds mnt->lock and an open transaction.
 */
static int bitterfs_add_dirent(struct bitter_trans *trans, struct inode *dir,
			       struct inode *inode, const char *name,
			       int name_len, bt_u64 index)
{
	struct bitterfs_mount	*mnt = bitterfs_sb(dir->i_sb);
	struct bitter_env	*env = &mnt->env;

	struct bitter_path	 path;
	struct bitter_key_cpu	 key;
	struct bitter_key_cpu	 target;
	struct bitter_dir_item	*di;

	/* Header plus the longest name the format allows.  A flexible array
	 * member cannot be a local, so the storage is declared as bytes and the
	 * struct is laid over it. */
	unsigned char		 buf[BITTER_DIR_ITEM_HEADER_SIZE +
				     BITTER_MAX_FILENAME];
	bt_u32			 size;
	int			 s;

	lockdep_assert_held(&mnt->lock);
	BITTER_ASSERT(name_len > 0 && name_len <= BITTER_MAX_FILENAME);

	di   = (struct bitter_dir_item *) buf;
	size = BITTER_DIR_ITEM_HEADER_SIZE + (bt_u32) name_len;

	/*
	 * `location` is the key of what this entry POINTS AT, not this item's
	 * own key -- the one field in the format most easily misread.  Only its
	 * objectid is consulted by lookup today; the type is written
	 * nonetheless, because an entry naming something other than an inode is
	 * how a subvolume would eventually be spelled.
	 */
	target.objectid = inode->i_ino;
	target.type     = BITTER_INODE_ITEM;
	target.offset   = 0;
	bitter_key_to_disk(&di->location, &target);

	bt_put_le16(&di->name_len, (bt_u16) name_len);

	/* The d_type readdir will report, derived from the new inode's mode.
	 * Stored rather than looked up so that a listing costs no inode reads at
	 * all -- which is the entire reason this byte is duplicated here. */
	di->type = bitterfs_ft(inode->i_mode);

	memcpy(di->name, name, (size_t) name_len);

	bitter_path_init(&path);

	/*
	 * DIR_ITEM first.  If the second insert fails, what survives is a name
	 * lookup can find but readdir will not list -- the file works and does
	 * not appear.  The other order gives a name that appears in ls and
	 * cannot be opened, which is the worse half of a bad pair.
	 */
	bitter_dir_item_key(&key, dir->i_ino,
			    bitter_name_hash(name, (bt_u32) name_len));

	s = btree_insert(env, &mnt->fs.fs_root, &key, &path, buf, size, trans);
	if (s == -BITTER_EEXIST) {
		/*
		 * NOT a duplicate name -- lookup already established this name
		 * does not exist.  Two DIFFERENT names hashed to one value, and
		 * keys are unique within a tree, so both entries cannot exist
		 * as separate items.
		 *
		 * btrfs packs several entries into one item and walks them
		 * comparing full names; this struct does not foreclose that and
		 * nothing implements it.  Well-formed and unimplemented, so
		 * -EOPNOTSUPP rather than -EUCLEAN.  See the note in
		 * format/bitterfs_format.h and docs/LOG.md.
		 */
		pr_err("dir %llu: name hash collision on \"%.*s\"\n",
		       (unsigned long long) dir->i_ino, name_len, name);
		return -EOPNOTSUPP;
	}
	if (s < 0) {
		return s;
	}

	/*
	 * DIR_INDEX second, same bytes, different key.  A collision here means
	 * next_dir_index handed out a value already in use -- corruption of the
	 * high-water mark rather than anything about names, which is why this
	 * one is -EUCLEAN where the other is -EOPNOTSUPP.
	 */
	bitter_dir_index_key(&key, dir->i_ino, index);

	s = btree_insert(env, &mnt->fs.fs_root, &key, &path, buf, size, trans);
	if (s == -BITTER_EEXIST) {
		pr_err("dir %llu: DIR_INDEX %llu already in use\n",
		       (unsigned long long) dir->i_ino,
		       (unsigned long long) index);
		return -EUCLEAN;
	}

	/* btree_insert released the path on every exit, both times. */
	return s;
}

/*
 * Make a regular file and attach it to `dir` under the name in `dentry`.
 *
 * INCOMPLETE: the tree writes are not here yet.  Everything up to trans_begin
 * is done; what is missing is the four inserts and the parent update.
 *
 * `excl` is ignored on purpose.  It says O_EXCL was given, and the VFS has
 * already done the lookup and established the name does not exist while
 * holding dir's i_rwsem exclusive -- so nothing can create it underneath us.
 * The flag exists for network filesystems that cannot rely on that.
 */
static int bitterfs_create(struct mnt_idmap *idmap,
                         struct inode *dir,
                         struct dentry *dentry, umode_t mode, bool excl) {

  struct super_block *sb;
  struct bitterfs_mount *mnt;
  struct inode* inode;
  struct timespec64 now;
  
  struct bitter_trans* trans;

  bt_u64 parent_index;

  const char* name;
  int name_len;
  int err;
  int s;

  sb = dir->i_sb;
  mnt = bitterfs_sb(sb);

  name = dentry->d_name.name;
  name_len = dentry->d_name.len;

  /* Bounded before it reaches a dir item, whose name_len field is the on-disk
   * record of this.  The VFS caps a component at NAME_MAX (255), which happens
   * to equal BITTER_MAX_FILENAME -- checked anyway, because equal-by-
   * coincidence is not the same as equal-by-construction. */
  if (name_len > BITTER_MAX_FILENAME) {
    return -ENAMETOOLONG;
  }

  /* First thing that can fail, and the one exit that owes nothing: no inode,
   * no lock, no transaction. */
  inode = new_inode(sb);
  if (!inode) {
    return -ENOMEM;
  }

  mutex_lock(&mnt->lock);

  /*
   * next_ino is shared mount state, so the read-modify-write needs mnt->lock
   * -- dir's i_rwsem does not cover it, and two creates in DIFFERENT
   * directories run concurrently.
   *
   * The bound is not about exhaustion by use: 2^64 creates is 584 years at a
   * billion a second.  It is about a number that arrived from the disk -- a
   * corrupt image can simply CONTAIN an inode numbered LAST_FREE, after which
   * fill_super's +1 is already out of range and the very first create hands
   * out an objectid from the region reserved for trees.
   */
  if (mnt->next_ino > BITTER_LAST_FREE_OBJECTID) {
    err = -ENOSPC;
    goto out_unlock_iput;
  }
  inode->i_ino = mnt->next_ino++;

  /* Ownership, and the setgid-inheritance rules, through the mount's idmap --
   * which is why ->create takes one at all.  Must come before the mode is read
   * back out, since it can adjust it. */
  inode_init_owner(idmap, inode, dir, mode);
  inode->i_size = 0;
  set_nlink(inode, 1);

  /*
   * All three of the new inode's timestamps, and the return is the moment they
   * were set.  Reused for the parent below so the file and the directory agree
   * exactly rather than differing by however long the code between them takes.
   *
   * The parent gets mtime and ctime only, never atime: atime means its
   * CONTENTS were read, and adding an entry is not a read.
   */
  now = simple_inode_init_ts(inode);
  inode_set_mtime_to_ts(dir, now);
  inode_set_ctime_to_ts(dir, now);

  /* The same tables iget installs for S_ISREG, and the two must agree
   * forever -- iget derives them from mode on the way in, this sets them on
   * the way out. */
  inode->i_op                = &bitterfs_file_inode_operations;
  inode->i_fop               = &bitterfs_file_operations;
  inode->i_mapping->a_ops    = &bitterfs_aops;

  /*
   * Hashes the inode so a later iget_locked finds THIS one rather than
   * building a second in-memory inode for the same number.  From here the
   * inode is I_NEW and the cleanup changes: discard_new_inode, never a bare
   * iput -- and never discard_new_inode before this point, which WARNs.
   *
   * -EBUSY means that number is already live in the cache, which cannot happen
   * with a monotonic counter seeded above the tree.  Passed through as-is: it
   * is a legible signal that inode-number allocation is wrong.
   */
  s = insert_inode_locked(inode);
  if (s < 0) {
    err = s;
    goto out_unlock_iput;
  }

  trans = bitterfs_trans_begin(mnt);
  if (IS_ERR(trans)) {
    /* Joined nothing, so no trans_end -- the one exit that skips that rung. */
    err = PTR_ERR(trans);
    goto out_unlock;
  }

  /*
   * The order is load-bearing.  The inode item goes in FIRST, so that a
   * partial failure leaves an ORPHAN -- an inode nothing names, which costs
   * space and nothing else -- rather than a dangling name that appears in ls
   * and cannot be opened.
   *
   * There is no transaction abort, so "partial failure" is a real outcome and
   * not a hypothetical: what stops it reaching the disk is bitterfs_commit
   * never running, because write_failed set SB_RDONLY and sync_filesystem
   * returns early on a read-only superblock.  Accidental, and the reason the
   * ordering above still matters.
   */
  s = bitterfs_insert_inode(trans, inode);
  if (s) {
    err = s;
    goto out_trans;
  }

  /* Must precede add_dirent, which needs the index to build a key.  A failure
   * after this leaves a gap in the sequence, which is harmless -- it is a
   * high-water mark, not a count. */
  s = bitterfs_take_dir_index(trans, dir, &parent_index);
  if (s) {
    err = s;
    goto out_trans;
  }

  /* Both dirents: (D, DIR_ITEM, hash) for lookup and (D, DIR_INDEX, idx) for
   * readdir, one payload inserted twice. */
  s = bitterfs_add_dirent(trans, dir, inode, name, name_len, parent_index);
  if (s) {
    err = s;
    goto out_trans;
  }

  /* The matching backref.  After the dirents rather than before, so that the
   * two orphan-shaped partial failures stay adjacent: an inode with no name,
   * or a name with no backref.  Both leak; neither dangles. */
  s = bitterfs_add_inode_ref(trans, dir, inode, name, name_len, parent_index);
  if (s) {
    err = s;
    goto out_trans;
  }

  /*
   * The parent's own item, last.  Only three fields move, and two of them were
   * set at the top of this function from `now`.
   *
   * i_size for a directory is the sum of its entries' name lengths -- the
   * convention mkfs established in format_initial_fs_tree.  Nothing reads it,
   * but a directory whose size never changes while entries are added looks
   * like a bug for a while before someone confirms it is not.
   *
   * next_dir_index is NOT written here: update_inode preserves it, and
   * take_dir_index has already stored the bumped value.
   */
  dir->i_size += name_len;

  s = bitterfs_update_inode(trans, dir);
  if (s) {
    err = s;
    goto out_trans;
  }
  
  /*
   * Success.  d_instantiate_new rather than d_instantiate: it clears I_NEW as
   * well as attaching the inode, and an inode left I_NEW puts every future
   * opener to sleep on that bit forever.
   *
   * After the unlock, because it can sleep and has no business holding a
   * filesystem lock.
   */
  s = bitterfs_trans_end(mnt);
  mutex_unlock(&mnt->lock);

  if (s) {
    /* The transaction failed to commit.  The inode is built and the tree is
     * consistent, but nothing reached the disk -- report it rather than
     * returning a file that will not survive. */
    err = s;
    clear_nlink(inode);
    discard_new_inode(inode);
    return err;
  }

  d_instantiate_new(dentry, inode);
  return 0;

  /*
   * The unwind ladder.  Labels fall through, so each is "undo everything from
   * here down", and they appear in reverse order of acquisition.
   */
  
out_trans:
  bitterfs_trans_end(mnt);
out_unlock:
  mutex_unlock(&mnt->lock);
  clear_nlink(inode);
  discard_new_inode(inode);
  return err;

  /* A separate bottom rather than another rung: before insert_inode_locked
   * succeeds the inode is neither hashed nor I_NEW, and discard_new_inode
   * WARNs on an inode that is not I_NEW. */
out_unlock_iput:
  mutex_unlock(&mnt->lock);
  iput(inode);
  return err;
}

/*
 * Make a directory and attach it to its parent.
 *
 * ->create's structure, and worth reading beside it: the five writes are the
 * same and the differences are all consequences of one fact -- a directory
 * contains "..", and a regular file does not.
 *
 *   S_IFDIR  the VFS passes permission bits without the type, so mkdir ORs it
 *            in.  ->create's mode arrives with S_IFREG already set.
 *   nlink 2  its entry in the parent, plus its own ".".
 *   inc_nlink(dir)  the new directory's ".." is a second link to the PARENT.
 *                   This is the change create deliberately does not make.
 *   no a_ops        a directory's contents are tree items, not page-cache
 *                   data, matching iget's S_ISDIR branch.
 *
 * Two things that look like they should differ and do not.  next_dir_index is
 * already handled, because bitterfs_insert_inode switches on S_ISDIR itself.
 * And the new directory needs only ONE backref, the same as a file: the ".."
 * relationship IS that backref's offset.  mkfs gives the root a
 * self-referencing one only because the root has no parent.
 *
 * There is no `excl` argument: mkdir on an existing name always fails at the
 * VFS, so there is no O_EXCL to honour.
 */
static int bitterfs_mkdir(struct mnt_idmap *idmap, struct inode* dir,
                        struct dentry *dentry, umode_t mode) {

  struct super_block *sb;
  struct bitterfs_mount *mnt;
  struct inode* inode;
  struct timespec64 now;
  
  struct bitter_trans* trans;

  bt_u64 parent_index;

  const char* name;
  int name_len;
  int err;
  int s;

  sb = dir->i_sb;
  mnt = bitterfs_sb(sb);

  name = dentry->d_name.name;
  name_len = dentry->d_name.len;

  if (name_len > BITTER_MAX_FILENAME) {
    return -ENAMETOOLONG;
  }

  inode = new_inode(sb);
  if (!inode) {
    return -ENOMEM;
  }

  mutex_lock(&mnt->lock);

  if (mnt->next_ino > BITTER_LAST_FREE_OBJECTID) {
    err = -ENOSPC;
    goto out_unlock_iput;
  }
  inode->i_ino = mnt->next_ino++;

  /* Ownership, and the setgid-inheritance rules, through the mount's idmap --
   * which is why ->create takes one at all.  Must come before the mode is read
   * back out, since it can adjust it. */
  /* S_IFDIR OR'd in: ->mkdir receives permission bits only. */
  inode_init_owner(idmap, inode, dir, mode | S_IFDIR);

  /* Empty: no entries, and a directory's size is the sum of its entries' name
   * lengths.  "." and ".." are synthesised by readdir and stored nowhere. */
  inode->i_size = 0;

  /* 2, not 1: its entry in the parent, plus its own ".". */
  set_nlink(inode, 2);

  /* All three of the new directory's timestamps, and the return is the moment
   * they were set -- reused for the parent below so the two agree exactly.
   *
   * The parent gets mtime and ctime only.  Never atime: that means its
   * contents were READ, and adding an entry is not a read. */
  now = simple_inode_init_ts(inode);
  inode_set_mtime_to_ts(dir, now);
  inode_set_ctime_to_ts(dir, now);

  /* No a_ops, unlike create -- see the note above.  These two match iget's
   * S_ISDIR branch, and the two must agree forever. */
  inode->i_op                = &bitterfs_dir_inode_operations;
  inode->i_fop               = &bitterfs_dir_operations;

  s = insert_inode_locked(inode);
  if (s < 0) {
    err = s;
    goto out_unlock_iput;
  }

  trans = bitterfs_trans_begin(mnt);
  if (IS_ERR(trans)) {
    /* Joined nothing, so no trans_end -- the one exit that skips that rung. */
    err = PTR_ERR(trans);
    goto out_unlock;
  }

  s = bitterfs_insert_inode(trans, inode);
  if (s) {
    err = s;
    goto out_trans;
  }

  s = bitterfs_take_dir_index(trans, dir, &parent_index);
  if (s) {
    err = s;
    goto out_trans;
  }

  /* Both dirents: (D, DIR_ITEM, hash) for lookup and (D, DIR_INDEX, idx) for
   * readdir, one payload inserted twice. */
  s = bitterfs_add_dirent(trans, dir, inode, name, name_len, parent_index);
  if (s) {
    err = s;
    goto out_trans;
  }

  s = bitterfs_add_inode_ref(trans, dir, inode, name, name_len, parent_index);
  if (s) {
    err = s;
    goto out_trans;
  }

  dir->i_size += name_len;

  /* LATE, deliberately.  Every failure path after this owes a drop_nlink, so
   * doing it here rather than up with the timestamps shrinks the unwind
   * surface to the two calls below.  Worth treating differently from a
   * timestamp: a stale mtime is cosmetic, a stale link count is a number the
   * VFS uses to decide whether an inode is alive, and rmdir will compare it
   * against 2 to decide whether this directory is empty. */
  inc_nlink(dir);

  s = bitterfs_update_inode(trans, dir);
  if (s) {
    err = s;
    goto out_droplink;
  }

  s = bitterfs_trans_end(mnt);

  if (s) {
    /*
     * The commit failed, so nothing reached the disk -- including the parent's
     * new link count, which is still incremented in memory.  Undone here for
     * the same reason the ladder undoes it: the value on disk is the old one,
     * and leaving memory ahead of it is how a later operation persists a
     * number nothing ever justified.
     */
    err = s;
    drop_nlink(dir);
    mutex_unlock(&mnt->lock);
    clear_nlink(inode);
    discard_new_inode(inode);
    return err;
  }

  mutex_unlock(&mnt->lock);

  d_instantiate_new(dentry, inode);
  return 0;

out_droplink:
  drop_nlink(dir);
out_trans:
  bitterfs_trans_end(mnt);
out_unlock:
  mutex_unlock(&mnt->lock);
  clear_nlink(inode);
  discard_new_inode(inode);
  return err;

  /* A separate bottom rather than another rung: before insert_inode_locked
   * succeeds the inode is neither hashed nor I_NEW, and discard_new_inode
   * WARNs on an inode that is not I_NEW. */
out_unlock_iput:
  mutex_unlock(&mnt->lock);
  iput(inode);
  return err;

}

/*
 * Remove a name from a directory.
 *
 * It does NOT delete the file.  An inode can reach nlink == 0 while still
 * open, and POSIX requires a held descriptor to keep working afterwards -- so
 * the extents and the inode item are freed by bitterfs_evict_inode, when the
 * last reference goes away.  This function removes three items and adjusts two
 * link counts; nothing it does scales with the file's size, which is why one
 * transaction is always enough where truncate needed a batch loop.
 *
 * The three deletes reverse create's three inserts, so a partial failure
 * leaves the same residue shape create's does: a name lookup can find that
 * readdir will not list.  One leftover shape rather than two.
 *
 * Absence anywhere below is -EUCLEAN, never -ENOENT.  ->unlink is only called
 * on a POSITIVE dentry, so lookup already found this name; reporting "no such
 * file" about a name ls is still showing would be a lie told to rm.
 */
static int bitterfs_unlink(struct inode* dir, struct dentry* dentry) {

  struct bitterfs_mount *mnt = bitterfs_sb(dir->i_sb);
  struct inode* inode = d_inode(dentry);
  struct bitter_env* env = &mnt->env;
  struct bitter_root* fs_root = &mnt->fs.fs_root;
  struct bitter_trans* trans;
  struct bitter_path path;

  struct bitter_item* it;
  struct bitter_dir_item* di;
  struct bitter_inode_ref* ir;

  struct bitter_key_cpu key;
  struct timespec64 now;

  bt_u64 index;
  bt_u32 size;
  bt_u16 name_len;
  int s;
  int r;
  int err;

  mutex_lock(&mnt->lock);

  bitter_path_init(&path);

  trans = bitterfs_trans_begin(mnt);
  if (IS_ERR(trans)) {
    /* Joined nothing, so no trans_end -- the one exit that skips that rung. */
    err = PTR_ERR(trans);
    mutex_unlock(&mnt->lock);
    return err;
  }

  /* --- 1. the DIR_ITEM: validate before deleting anything ---------------
   *
   * The key is computable from the name alone, so this search is purely for
   * the checks below.  They cannot be skipped: unlike lookup, where believing
   * the wrong item returns the wrong inode, here it would DELETE the wrong
   * name, and nothing recovers that.
   */
  bitter_dir_item_key(&key, dir->i_ino,
                      bitter_name_hash(dentry->d_name.name,
                                       (bt_u32) dentry->d_name.len));

  s = btree_search(env, fs_root, &key, &path, 0, NULL, 0);
  if (s != 0) {
    err = (s < 0) ? s : -EUCLEAN;
    goto out_trans;         /* search released the path itself */
  }

  it   = bitter_leaf_item(path.nodes[0]->b_data, (bt_u32) path.slots[0]);
  size = bt_get_le32(&it->size);

  /* Order is forced: name_len lives inside the payload, so the item has to be
   * known big enough to hold a header before that field is read. */
  if (size < BITTER_DIR_ITEM_HEADER_SIZE) {
    pr_err("dir %llu: entry item is %u bytes, under the %d-byte header\n",
           (unsigned long long) dir->i_ino, size,
           BITTER_DIR_ITEM_HEADER_SIZE);
    err = -EUCLEAN;
    goto out_release;
  }

  di       = bitter_leaf_data(path.nodes[0]->b_data, (bt_u32) path.slots[0]);
  name_len = bt_get_le16(&di->name_len);

  if (name_len > BITTER_MAX_FILENAME ||
      size != BITTER_DIR_ITEM_HEADER_SIZE + name_len) {
    pr_err("dir %llu: name_len %u does not match item size %u\n",
           (unsigned long long) dir->i_ino, name_len, size);
    err = -EUCLEAN;
    goto out_release;
  }

  /*
   * The KEY matching means the HASH matched.  lookup answers a mismatch with
   * "not found" because a collision genuinely means the name is absent; here
   * it is -EUCLEAN, because the positive dentry proves lookup already matched
   * this name against this item.  If it no longer does, the tree moved
   * underneath the dcache.
   *
   * Length first, which is what bounds the memcmp safely.
   */
  if (name_len != dentry->d_name.len ||
      memcmp(di->name, dentry->d_name.name, name_len) != 0) {
    pr_err("dir %llu: entry at this hash is not \"%.*s\"\n",
           (unsigned long long) dir->i_ino,
           (int) dentry->d_name.len, dentry->d_name.name);
    err = -EUCLEAN;
    goto out_release;
  }

  /* The check lookup does not need.  location is a KEY, and only its objectid
   * is read; an entry naming a different inode than the dcache does means
   * removing it would unlink the wrong file. */
  if (bt_get_le64(&di->location.objectid) != inode->i_ino) {
    pr_err("dir %llu: entry \"%.*s\" names inode %llu, not %llu\n",
           (unsigned long long) dir->i_ino,
           (int) dentry->d_name.len, dentry->d_name.name,
           (unsigned long long) bt_get_le64(&di->location.objectid),
           (unsigned long long) inode->i_ino);
    err = -EUCLEAN;
    goto out_release;
  }

  bitter_path_release(env, &path);

  /* --- 2. the backref: read the DIR_INDEX out of it ---------------------
   *
   * This search is MANDATORY, not a check.  The backref carries the index the
   * entry was given, and that value exists nowhere else -- without it there is
   * no way to name the DIR_INDEX item still to be deleted.
   */
  bitter_inode_ref_key(&key, inode->i_ino, dir->i_ino);

  s = btree_search(env, fs_root, &key, &path, 0, NULL, 0);
  if (s != 0) {
    pr_err("inode %llu: no backref in dir %llu\n",
           (unsigned long long) inode->i_ino,
           (unsigned long long) dir->i_ino);
    err = (s < 0) ? s : -EUCLEAN;
    goto out_trans;
  }

  it   = bitter_leaf_item(path.nodes[0]->b_data, (bt_u32) path.slots[0]);
  size = bt_get_le32(&it->size);

  if (size < BITTER_INODE_REF_SIZE) {
    pr_err("inode %llu: backref is %u bytes, under the %d-byte header\n",
           (unsigned long long) inode->i_ino, size, BITTER_INODE_REF_SIZE);
    err = -EUCLEAN;
    goto out_release;
  }

  /* Copied out before the release: it points into the leaf's buffer, and the
   * CoW descents below will copy and relink the blocks this path holds. */
  ir    = bitter_leaf_data(path.nodes[0]->b_data, (bt_u32) path.slots[0]);
  index = bt_get_le64(&ir->index);

  bitter_path_release(env, &path);

  /* --- 3. the three deletes, reversing create's inserts ------------------
   *
   * btree_del_item does its own search, its own dirty_block and fixup, and
   * releases the path on every exit -- so nothing is owed after each, and one
   * path variable serves all three.
   *
   * -BITTER_ENOENT from any of them means the key lookup above disagreed with
   * the tree, which is the same -EUCLEAN story as everywhere else here.
   */
  bitter_dir_index_key(&key, dir->i_ino, index);
  s = btree_del_item(env, fs_root, &key, &path, trans);
  if (s < 0) {
    err = (s == -BITTER_ENOENT) ? -EUCLEAN : s;
    goto out_trans;
  }

  bitter_dir_item_key(&key, dir->i_ino,
                      bitter_name_hash(dentry->d_name.name,
                                       (bt_u32) dentry->d_name.len));
  s = btree_del_item(env, fs_root, &key, &path, trans);
  if (s < 0) {
    err = (s == -BITTER_ENOENT) ? -EUCLEAN : s;
    goto out_trans;
  }

  bitter_inode_ref_key(&key, inode->i_ino, dir->i_ino);
  s = btree_del_item(env, fs_root, &key, &path, trans);
  if (s < 0) {
    err = (s == -BITTER_ENOENT) ? -EUCLEAN : s;
    goto out_trans;
  }

  /* --- 4. the file: one fewer name --------------------------------------
   *
   * drop_nlink rather than set_nlink(inode, i_nlink - 1): the VFS has to see
   * the transition to zero, because that is what makes the later iput reach
   * evict_inode and actually delete the file.
   *
   * ctime, not mtime: the inode's METADATA changed -- its link count -- while
   * its contents did not.
   */
  now = current_time(inode);

  drop_nlink(inode);
  inode_set_ctime_to_ts(inode, now);

  s = bitterfs_update_inode(trans, inode);
  if (s) {
    err = s;
    goto out_trans;
  }

  /*
   * --- 5. the parent -----------------------------------------------------
   *
   * No nlink change: a regular file contributes no link to its parent, which
   * is the exact mirror of create declining to inc_nlink.  rmdir is where the
   * parent's count moves, because a subdirectory's ".." is a real link.
   *
   * The same `now` as the file above, so the two agree about when this
   * happened rather than differing by however long the code between them runs.
   */
  dir->i_size -= name_len;
  inode_set_mtime_to_ts(dir, now);
  inode_set_ctime_to_ts(dir, now);

  s = bitterfs_update_inode(trans, dir);
  if (s) {
    err = s;
    goto out_trans;
  }

  err = bitterfs_trans_end(mnt);
  if (err) {
    bitterfs_write_failed(mnt, "unlink", err);
  }

  mutex_unlock(&mnt->lock);
  return err;

  /*
   * The ladder.  out_release is a rung rather than a separate bottom: every
   * validation failure above happens with the path HELD, and falling through
   * to out_trans without releasing would leak a buffer at every level.
   */
out_release:
  bitter_path_release(env, &path);
out_trans:
  r = bitterfs_trans_end(mnt);
  if (!err)
    err = r;
  bitterfs_write_failed(mnt, "unlink", err);
  mutex_unlock(&mnt->lock);
  return err;
}

/*
 * ->rmdir and ->rename stay NULL, which is
 * what makes the VFS return -EPERM for them rather than calling through a null
 * pointer -- a read-only filesystem needs no stubs that refuse.
 */
const struct inode_operations bitterfs_dir_inode_operations = {
	.lookup  = bitterfs_lookup,
	.create  = bitterfs_create,
	.mkdir   = bitterfs_mkdir,
	.unlink  = bitterfs_unlink,

	/* Shared with regular files -- see bitterfs_file_inode_operations.
	 * No .getattr: with it NULL the VFS falls back to generic_fillattr,
	 * which reads the fields iget already filled in. */
	.setattr = bitterfs_setattr,
};

const struct file_operations bitterfs_dir_operations = {
	.llseek		= generic_file_llseek,

	/* Returns -EISDIR.  Without it, read() on a directory does something
	 * other than fail. */
	.read		= generic_read_dir,

	.iterate_shared	= bitterfs_readdir,

	/* The same function files use.  fsync(dirfd) after a rename is how a
	 * program makes a name durable, and a missing member here is -EINVAL
	 * rather than a no-op. */
	.fsync		= bitterfs_fsync,

	/* Nothing to flush on a read-only mount, but without SOME fsync the
	 * VFS returns -EINVAL for fsync() on a directory. */
	.fsync		= noop_fsync,
};
