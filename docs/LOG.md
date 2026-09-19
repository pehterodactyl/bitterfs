# LOG

Running notebook: what broke, what was learned, and what was left undone on
purpose. Three sections — open debt, bugs worth remembering, and debts since
paid. Newest first within each.

A paid debt keeps its entry rather than being deleted: the reasoning that made
it acceptable at the time is the part worth re-reading, and knowing what
finally forced the fix is what makes the next estimate honest.

---

## Open debt

Each entry says what is missing, why it was acceptable to leave it, and what
will force the fix. A debt with no third line is a bug that has not been
admitted yet.


### The commit failure policy is written out three times

*Incurred: phase 7, 2026-09-16.*

Three functions now say what happens when a write fails, and they say it
separately.

`bitterfs_trans_end` and `bitterfs_sync_fs` both call `trans_commit` and then
carry the same two-sided aftermath: on failure set `SB_RDONLY`, return the
error and leave `trans_live` set so nothing builds on in-memory state that is
ahead of the disk; on success clear `trans_live` so the next `trans_begin`
starts a transaction rather than joining a published one. `bitterfs_setattr`
carries the failure half only, because `setattr_copy` has already changed the
cached inode by the time anything can fail -- so an error there leaves memory
and disk disagreeing with nothing left to find them by.

They already differ. `trans_end` and `sync_fs` guard on `err < 0`; `setattr`
guards on `err`. Identical today, since every error return in the module is
negative, and not identical by construction.

**Why it was acceptable:** all three live in one module, all three currently
agree, and the shape a helper should take depends on callers that do not exist
yet. Truncate, create and unlink each want a transaction and each will want an
opinion about failure; designing the signature against one caller and two
guesses is how a helper acquires parameters nobody needs.

**What will force it:** the failure policy changing. `SB_RDONLY` plus a live
`trans_live` is a stand-in for a real errored mount state -- the deferral
already recorded against `bitterfs_trans_end`. The day that arrives, it has to
land in three places on the same afternoon or the policy stops being one
policy, and the site that gets missed is the one with no test behind it,
because a commit failure is not something the smoke harness can currently
provoke.

### A failed assertion wedges the mount until reboot

*Incurred: phase 5, 2026-09-09.*

`kernel/assert.c` defines `bitter_assert_fail()` as `pr_emerg()` followed by
`BUG()`. `BUG()` plants a `ud2`, and the trap handler prints the message,
dumps registers and a stack trace, and kills the **task** — not the machine.
`CONFIG_PANIC_ON_OOPS` is not set in the pinned config, so that is what happens.

Nothing unwinds. bitterfs serialises every entry point behind one
filesystem-wide mutex, and a task killed inside `core/` never releases it. After
an assertion fires the guest is still up and `dmesg` still has the report, but
that mount is unusable: no further access, no unmount, no `rmmod`.
`CONFIG_DETECT_HUNG_TASK=y`, so it also produces a hung-task splat every 120
seconds, burying the assertion message that was the point.

The alternative was `panic()`, which stops the machine cleanly and cannot leave
a lock held because there is nothing left to hold it. Rejected because the guest
staying alive is worth more during development: the report can be read, copied
out over the 9p share, and poked at, and the guest is disposable anyway.

Also worth recording: the file and line `CONFIG_DEBUG_BUGVERBOSE` prints come
from `__bug_table`, populated at the site of the `BUG()` macro. That site is
`kernel/assert.c` and says so every time, whichever invariant broke. The `expr`,
`file` and `line` arguments are the only carriers of the real location, which is
why the function prints them itself rather than calling `BUG()` bare.

**What forces it:** an assertion that fires somewhere reboot-hostile — a test
loop the crash work needs to run hundreds of times, or a failure during
unmount where the hang is the only symptom and the message scrolls away. At
that point this becomes `panic()`, or the assertion handler learns to release
the mutex before trapping.

### The block checksum has to move when writes arrive

*Incurred: phase 5, 2026-09-10.*

In userspace, `put_block` is the write path. `user/env_user.c` computes a
block's checksum there, with the comment: *"The only place a tree block is
written, so the only place its checksum needs computing. core/ never maintains
it: a block is modified many times between the CoW that created it and the
write that ends its life in memory, and only the last of those states goes to
disk."*

The kernel cannot do that. `mark_buffer_dirty(bh)` hands the buffer to the
kernel's writeback machinery, which may write it at any moment afterwards
without passing through module code again. So the checksum has to already be
correct at the instant `dirty_block` returns — not at `put_block`.

Which collides with the sentence above. `core/` calls `dirty_block` repeatedly
on the same buffer between a CoW and its release, so the kernel either
recomputes the checksum on every one of those calls, or delays
`mark_buffer_dirty` until `put_block` and accepts a different set of tradeoffs
around writeback latency and dirty accounting.

Acceptable to leave because phase 5 is read-only: `bitterfs_dirty_block` is a
`BITTER_ASSERT(0)` and nothing reaches it. The assertion is the marker.

**What forces it:** phase 7, the first write. This is the first concrete
instance of what the skeleton calls that phase's central difficulty — "where
the page cache and copy-on-write collide" — and it is worth recognising as that
rather than rediscovering it as a checksum bug.

### `core/` has only ever run against an environment with no buffer cache

*Incurred: phase 5, 2026-09-10.*

`user/env_user.c` says of itself: *"Deliberately dumb: no buffer cache, so two
read_block calls for the same block give two independent copies."* Every unit
test, every fixture, and `bitter-fsck` all run against that environment.

`env_kernel.c` is not dumb, because `sb_bread` is a cache lookup first and a
read second. Two `read_block` calls for the same `bytenr` in the kernel return
the same `buffer_head` and the same `b_data` — two `bitter_buf`s aliasing one
buffer. Modifying through one is visible through the other; releasing one does
not invalidate the other.

`core/` must be correct under both, assuming neither aliasing nor independence.
Nothing verifies that it is. Any place it holds two buffers for the same block
at once — a path and a sibling during a split, a node and its child — behaves
differently in the two environments, and only one of them is tested.

Acceptable because read-only mounts do not modify blocks, so aliasing is
harmless today: two readers of identical bytes.

**What forces it:** phase 7. The moment a write path exists, two aliasing
buffers for one block are two views of state being changed, and the tests
cannot see it. The fix is either a caching test environment in `user/` or an
explicit rule in `core/` that a block is fetched once per path.

### `iget_locked` truncates a 64-bit objectid on 32-bit

*Incurred: phase 5, 2026-09-11.*

The VFS inode cache is keyed on `(struct super_block *, unsigned long ino)`, and
`iget_locked`'s parameter is `unsigned long`. bitterfs objectids are 64 bits,
from `bitter_key.objectid`.

On x86-64, `unsigned long` is 64 bits and nothing is lost. On a 32-bit build,
two objectids differing only above bit 32 truncate to the *same cache key* — and
the second file silently gets the first file's `struct inode`. Not an error, not
a wrong field: the wrong file, with the right file's mode, size and link count.

btrfs uses `iget5_locked`, which takes a comparison callback and can therefore
match on the full 64-bit value regardless of what `unsigned long` holds. That is
the fix, and it is contained: `bitterfs_iget` gains a `test` and a `set`
function and passes the objectid through as callback data.

Acceptable because `config.mk` pins the kernel tree and the project targets
x86-64 only, so the truncating case cannot currently be built.

**What forces it:** anyone building this for 32-bit, or the day `config.mk`'s
pinned tree changes architecture. Neither is on the roadmap, which is precisely
why it needs writing down rather than remembering.

### A directory entry item holds exactly one entry

*Incurred: phase 6, 2026-09-11.*

`DIR_ITEM` entries are keyed `(dir_ino, DIR_ITEM, hash(name))`. Two different
names can hash to the same value, and keys are unique within a tree — so both
entries would need to be the same item, and `struct bitter_dir_item` describes
exactly one.

btrfs handles this by packing several entries into one item and walking them
linearly, comparing full names. That is also why the name has to be in the
payload at all: the key cannot distinguish colliding names, so the reader must.

The struct does not foreclose the fix. An item can hold a sequence of
header-plus-name pairs with no layout change, because `name_len` gives the
stride and `bitter_item.size` gives the end. What is missing is the code: a
writer that appends rather than replaces, and a reader that loops rather than
casts.

Acceptable because nothing creates files. `mkfs` writes the entries it writes
and a collision among them would be a bug in `mkfs`, not a runtime condition.

**What forces it:** phase 7's `create`, where two colliding names is a thing a
user can do on purpose. With crc32c over the name, a deliberate collision is
findable in seconds, so this is a correctness bug and not merely a rare one —
`create` must either pack, or detect the collision and refuse, and refusing is
a filesystem that rejects a legal filename for no reason the user can see.

### File data is the one thing on the device nothing verifies

*Incurred: phase 6, 2026-09-14.*

Every block this filesystem reads goes through `env_kernel.c`'s `read_block`,
which checks three things before believing it: the checksum, the block's own
recorded `bytenr`, and the `fsid`. A block that is intact, in the right place
and belongs to this filesystem is the only kind `core/` ever sees.

A data block has none of that. No `bitter_header`, no checksum, nothing
self-identifying — it is bytes, and `mkfs` writes it as bytes. Nothing points
at it with a `key_ptr` either; the reference lives in an `EXTENT_DATA` item's
payload.

So `bitterfs_get_block` computes a device address from three disk-supplied
numbers and hands it to the block layer, which reads it and gives it to
userspace as file contents. A corrupt `disk_bytenr` pointing into the extent
tree would serve metadata as file data, and nothing downstream would notice.

What guards it today is one check in `get_block`: the computed address must be
block-aligned and inside `total_bytes`. That catches a wild value and catches
nothing subtler — an address that is plausible and wrong passes.

Acceptable because the alternative is real work: a checksum per data block
means a place to put it, which means either an `EXTENT_CSUM` item type (the one
`bitterfs_format.h` already reserves numbering room for) or a per-extent field,
plus verifying it on every read.

**What forces it:** wanting to trust the filesystem. btrfs made data checksums
default for the same reason, and its `EXTENT_CSUM` tree is what the gap in the
key-type numbering is for. Until then `bitter-fsck` cannot detect data
corruption at all, only metadata corruption — which is worth knowing when it
reports zero problems.

### readdir re-searches the tree for every entry

*Incurred: phase 6, 2026-09-14.*

`bitterfs_readdir` loops: take the lock, search for `(dir, DIR_INDEX,
ctx->pos)`, extract one entry, release, unlock, `dir_emit`. One B-tree descent
per directory entry.

The reason is not laziness. `dir_emit` can fault — `filldir64` writes to a user
buffer, and if that buffer is mapped from a file on this same filesystem,
faulting it in re-enters the filesystem. Holding `mnt->lock` across it is a
self-deadlock. btrfs says so in a comment above its own readdir: *"All this
infrastructure exists because dir_emit can fault, and we are holding the tree
lock when doing readdir."*

btrfs's answer is to batch — collect many entries into a temporary buffer under
the lock, drop it, then emit them all. That is the optimisation, and it costs a
buffer to size and manage plus a second function.

Acceptable because a descent on a cached leaf is a few comparisons and no I/O,
and because the simple version is obviously correct in a way the batched one
would have to be argued for.

**What forces it:** a directory large enough for the descents to show up, and a
profile that says they do. Not a guess — measure before adding the buffer.

### `iget_failed` now runs inside the caller's lock

*Incurred: phase 6, 2026-09-11.*

Moving `mnt->lock` out of `bitterfs_iget` and up to the VFS entry points fixed
the self-deadlock `lookup` would have hit. It also moved `iget_failed` from
outside the lock to inside it, because `iget`'s failure label no longer unlocks
anything — the caller does, after it returns.

`iget_failed` ends in `iput`, and an `iput` that drops the last reference
reaches `evict_inode`. There is no `evict_inode` yet, so the generic path runs
and nothing re-enters the filesystem.

**What forces it:** phase 7, where `evict_inode` becomes a real method that
frees blocks and will want the mutex it is already being called under. The fix
is to have the entry points release before the failure path, or to give `iget`
back a narrow acquisition for that one call. Decide it when the method exists
rather than now.

### A failed commit forces read-only rather than a real errored state

*Incurred: phase 7, 2026-09-15.*

When `->sync_fs` cannot commit, the filesystem sets `SB_RDONLY` and returns the
error. Nothing else changes.

On disk that is already correct and needs no action: the superblock was never
replaced, so the device still describes the previous transaction. Every block
the failed commit wrote is unreachable, and its allocations were delayed refs
that died in memory. There is nothing to roll back, which is the property
copy-on-write was for.

The danger is the **in-memory** state, which is now ahead of the disk.
`fs->tree_root.bytenr` points at blocks that may not be durable, some of the
transaction's buffers reached the device and some did not, and the delayed refs
were partially drained into an extent tree that exists only in memory.
Continuing would build the next transaction on a foundation that is not there —
and *that* commit could succeed, publishing a superblock naming blocks nobody
ever wrote. A harmless failure becomes corruption one transaction later.

So the requirement is to stop, and `SB_RDONLY` stops the write paths because
they already check it. That is the whole of the fix.

What it does NOT do is stop **reads**. The mount keeps serving from in-memory
roots that may name blocks the device does not have, so a read can return an
I/O error, or bytes from a block the extent tree no longer agrees about.
btrfs's answer is a filesystem-wide error state that refuses both directions and
is sticky across everything until unmount.

Also unhandled: the one commit failure that is genuinely ambiguous. `user/trans.c`
already describes it — a failure of the SECOND flush means *"the superblock
reached the page cache, and whether it survives a power cut is unknown. A blind
retry commits twice."* Read-only is a defensible response to that, since it
neither retries nor proceeds, but it is defensible by accident rather than by
design.

Acceptable because a commit failure means the device is failing, and a failing
device makes the reads suspect regardless of what the filesystem does. The
bounded, knowable outcome — everything since the last good superblock is gone —
is already the honest one.

**What forces it:** wanting to run this under a workload where a transient I/O
error is survivable rather than fatal, or phase 9's crash testing, where
deliberately failing a write and watching what the mount does next is the point
of the exercise.

### `trans_commit` names the roots it updates one by one

*Incurred: phase 7, 2026-09-15.*

The commit's drain loop calls `bitter_update_root` for `fs->extent_root` and
for `fs->fs_root`, by name, and tests both for movement before deciding it has
converged. Adding a tree to the filesystem means adding it here too.

Phase 4 wrote the loop when the extent tree was the only second tree, and it
stayed correct for two phases because nothing else was ever modified. Phase 7's
first write broke it: a `setattr` copy-on-writes the FS tree, its root moves,
and a root that moves without being recorded is **not an error**. The old tree
is still perfectly valid — that is what copy-on-write guarantees — so the commit
succeeds, the filesystem mounts, `bitter-fsck` reports zero problems, and the
change is simply gone.

Silent loss is the worst shape a bug can take here, and this one is reachable
by forgetting a line.

Acceptable now because there are exactly three trees and all three are named in
one place, which is at least greppable.

**What forces it:** phase 8. A snapshot is a new FS tree, there may be many, and
they are created at runtime rather than named in a struct — so there is nothing
to add a line for. btrfs tracks a LIST of dirty roots and updates whatever is on
it, which is the shape this has to become: a root registers itself as dirty when
`btree_cow_block` moves it, and the commit drains the list rather than
enumerating fields of `bitter_fs_info`.

### fsck checks the extent tree and nothing else

*Incurred: phase 4, 2026-09-08.*

`bitter-fsck` walks every block of every tree, records a reference per pointer,
and compares that against the extent tree's counts. It reports a wrong count, a
leaked extent, a block referenced with no extent item, a wrong level, a stale
generation, a foreign owner, and whatever the leaf and node checkers find.

What it does not check is everything that is not a refcount. Nothing verifies
that the root tree's root items agree with the trees they name, that
`super.bytes_used` bears any relation to the extent tree, that free ranges do
not overlap allocated ones, or that a key appears only once across a tree
rather than only within a leaf. Each of those is a real way an image can be
wrong and be pronounced clean.

Acceptable because the refcount is the one that destroys data silently, which
is why the skeleton put fsck in this phase rather than phase 8. The rest are
findable by other means: `bitter-dump` shows the root items, and a wrong
`bytes_used` announces itself at the next allocation.

**What forces it:** phase 8, where fsck stops being a refcount checker and
becomes a whole-image checker. Also phase 6 -- a snapshot makes the root tree
interesting, and an unchecked root item then points at a tree nothing verifies.

### fsck cannot repair anything

*Incurred: phase 4, 2026-09-08.*

Every finding is printed and counted; none is fixed. The exit status follows
e2fsck's convention with a deliberate gap -- 0 for clean and 4 for errors left
uncorrected, and no 1, because 1 means "problems were corrected" and that can
never happen yet.

This is the right order. A repair that is wrong is worse than no repair, and
the way to earn confidence in a repair is to have a detector you already trust.
The tool is also opened read-only, so it structurally cannot damage the
evidence it was run to find -- which stops being true the day it repairs.

**What forces it:** nothing yet. It becomes urgent the first time a real image
matters more than the exercise of rebuilding it.

### Freed space is not reclaimed until the next mount

*Incurred: phase 4, 2026-09-07.*

`extent_dec_ref` removes an extent's item when its count reaches zero, so the
extent tree stops calling that range allocated. Nothing puts the range back
into `fs->free`. The map only ever shrinks during a mount; it regains space
when `extent_build_free_map` rebuilds it, which happens once, at
`bitter_fs_info_init`.

So a long-running mount that deletes heavily can report ENOSPC with most of the
device unreferenced.

This is deliberate, and the alternative is worse. A block freed during a
transaction is still referenced by the COMMITTED on-disk filesystem — the one
`super.root` points at, the one that survives a crash right now. Returning it
to the map lets the allocator hand it out and overwrite it; a crash before the
superblock write then leaves the old, intact tree reading a block containing
something else. A filesystem that was consistent is destroyed by a transaction
that never completed.

btrfs solves that by pinning: extents freed during a transaction go to a
separate set and only join the free map after the commit completes. Never
returning space during a mount makes the same hazard structurally impossible
instead of carefully avoided, which is the right trade while the mechanism that
would get it right does not exist.

The specific temptation to resist is calling `extent_build_free_map` at the end
of `extent_apply_delayed_refs`. The map it produced would be perfectly
consistent with the extent tree, and it would resurrect every block freed this
transaction — into a transaction that is still allocating, since
`bitter_update_root` runs afterwards.

**What forces it:** pinning, or a rebuild placed after `trans_commit`'s second
`flush_device`, which is the first moment the new tree is the committed one.
Neither is needed until a workload both frees and allocates heavily inside one
mount, which nothing does yet.

### `trans_commit` has a branch that exists only for the fixtures

*Incurred: phase 4, 2026-09-07.*

The commit path skips the delayed-ref drain and the extent-root update when
`fs->extent_root.bytenr == 0`, and discards the pending set instead. No image
mkfs writes is ever in that state — it always creates an extent tree — but most
unit fixtures are: `bt_fixture_open` builds a root tree by hand and never makes
a second one.

So production code carries a conditional whose only purpose is to tolerate test
images, and the path real filesystems take is the one the branch skips over.
Worse, the condition doubles as an uninitialised-memory tripwire: it is only
correct because two places now `memset` the `fs_info` first, and forgetting a
third would read stack garbage and take whichever branch it happened to find.

Accepted because the alternative was worse *today*: giving the fixtures a real
extent tree collides with the tests' own keys. `bt_key(2)` is
`(2, INODE_ITEM, 0)` and the extent tree's root item is
`(2, ROOT_ITEM, 0)`, so `bt_collect_keys` counts 301 items and `bt_check_tree`
reports "key 2 follows 2". That was tried and reverted on 2026-09-06.

**What forces it:** teaching `bt_collect_keys` and `bt_check_tree` to filter on
item type, which is what they meant all along — they exist to check the items
the *test* inserted. With that, `bt_fixture_mkfs` can build the same three
blocks mkfs does, the fixtures stop being a special shape, and the branch goes
away.

### `btree_next_leaf` has never taken its multi-level path

*Incurred: phase 4, 2026-09-07.*

The walk is up-then-down: climb until a level has an unvisited slot, then
descend leftmost back to level 0. Every test and every driver so far has run
against a level-1 extent tree, where the branch point is always the root and
the descent runs exactly once. The loop that matters — descending two or more
levels after stepping a slot — has not executed.

That is the half the first version got wrong: it repopulated only the level it
found the branch point at and left `path->nodes[0]` pointing at a released
buffer. The rewrite handles it; nothing has proved it.

Reaching level 2 needs roughly 14,000 items, which is a fixture that builds a
tree directly rather than a driver that inserts.

**What forces it:** `user/fsck.c`, which walks whole trees for a living, or a
`tests/unit/next_leaf` built on `bt_build_node`. The second is cheaper and
should come first.

### The crash test cannot see flush ordering

*Incurred: phase 3, 2026-09-03.*

`unit/crash` simulates a power cut by copying the image at two points and
checking that each copy mounts as either the old filesystem or the new one. It
proves the property that matters most — blocks on disk are inert until the
superblock names them, so a crash in the window between them loses the
transaction and keeps the filesystem — and it proves the abandoned space is
reusable by a retry.

It cannot prove the flush ordering. `env_user` pwrites on `put_block`, so
"written" and "durable" are the same event in this environment, and a file copy
sees everything regardless of `fsync`. Deleting both `writeback_all` and the
first `flush_device` from `trans_commit` leaves the test green.

So the two seam ops are exercised but not verified. Their contract is real —
the kernel environment genuinely defers, and there `writeback_all` is where the
writes happen — but nothing here would notice if `trans_commit` stopped calling
them.

**What forces it:** either the kernel environment, where the ordering becomes
observable for free, or a write-injecting `env_user` that drops every write
after the Nth and lets the test sweep N across a whole commit. The second is a
day's work and would turn a two-point check into a systematic one.

### `super.bytes_used` carries the allocator's high-water mark

*Incurred: phase 3, 2026-09-03.*

`struct bitter_root` holds `next_free`, the bump allocator's high-water mark,
and nothing persisted it. A reopened image would restart the allocator wherever
it was initialised and hand out addresses that already held live blocks — a gap
no test had met, because every fixture sets `next_free` by hand and nothing has
ever reopened an image.

Rather than add a field, the mark now lives in `super.bytes_used`. With a bump
allocator "bytes allocated" and "the address above which everything is free"
are the same quantity, differing only by the 64 KiB reserved before the
superblock. The field is written as an address, not a count, so that restoring
`root->next_free` needs no arithmetic — any conversion is a place to get the
reserved prefix wrong once and allocate over the superblock.

The cost is a field whose name no longer quite describes it. `statfs` must
derive its figure rather than report the value directly.

**What forces it:** phase 4, and it has now half arrived. The extent tree
describes exactly which ranges are allocated, mkfs records the blocks it
writes, and `extent_build_free_map` derives free space from that alone — so
nothing at mount needs this field any more. It is no longer merely misnamed, it
is redundant.

What keeps it alive is only the vestigial `root->next_free` it feeds. The two
go together, or neither does.

### `btree_cow_block` has no refcount case

*Incurred: phase 3, 2026-09-03.*

The skeleton names three cases: already ours (modify in place), shared with a
snapshot (copy, do **not** free the original), and otherwise (copy, original
freed at commit). The code has two. There is no refcount to read, because there
is no extent tree; and there is no free at commit, because the bump allocator
cannot free anything at all.

Both missing halves are the same missing thing, which is why this is one entry
rather than two.

**What forces it:** phase 4. The moment a second tree can point at a block, the
"otherwise" case stops being safe, and it will be silently unsafe — copying a
block whose original is still referenced does no visible damage until the
original is reused.

**Half paid: 2026-09-07.** The "otherwise" case works. `btree_cow_block`
records a `-1` for the block it abandons through the delayed-ref set, so the
recursion is avoided and the block is genuinely freed at commit — a 300-insert
transaction on a fresh image now leaves two free ranges where there was one,
the reclaimed pair being the root tree's and extent tree's original leaves.

The SHARED case is still absent, and now it is the only one. Telling "shared"
from "otherwise" means reading the refcount from inside `btree_cow_block`,
which descends the extent tree, which copy-on-writes it, which is the same
recursion the delayed refs solved for writes but not for reads. Recording `-1`
unconditionally is correct only because every tree block currently has exactly
one parent.

**What forces the rest:** phase 6. A snapshot is the first thing that makes a
second reference to a tree block, and from that moment an unconditional `-1`
frees a block another tree is still using.

### A failed split can leak a block

*Incurred: phase 2.*

`btree_split_node`, when it grows a new root, commits `root->bytenr` and
`root->level` before allocating the sibling. If that allocation then fails, it
returns an error having already installed the new root. The tree is valid — the
new root has one child and the old root beneath it — but the block allocated
for it is unreachable from any subsequent free, because the bump allocator has
no free.

Correctness is not affected. Only space is, and only on a path that requires
running out of device in the middle of a split.

**What forces it:** the extent tree, which makes leaked blocks measurable and
`bitter-fsck` able to report them. Until allocation can be undone, the
alternative — unwinding the root installation — costs more complexity than the
leak costs bytes.

### Emptied leaves stay in the tree

*Incurred: phase 2.*

`btree_del_item` removes the last item from a leaf and leaves the leaf in
place, still pointed at by its parent, still holding a block.

It is safe because a separator is a *lower bound on a range*, not a claim that
an item exists: a search that routes into an empty leaf correctly reports
not-found, and a later insert into that range lands in it correctly. The
`del_item` test drains the tree to empty and refills it on a different stride
precisely to hold that property down.

The cost is shape, not correctness. Enough deletes fill nodes with pointers to
nothing and force splits that deepen a tree holding fewer items.

**What forces it:** nothing yet. It becomes visible as a performance problem
long before it becomes a correctness one, which is exactly why it needs writing
down rather than remembering.

### No merging or balancing on delete

*Incurred: phase 2.*

Deletion never merges an underfull block with a sibling and never rebalances.
A tree that has seen many deletes is taller and sparser than its item count
warrants, permanently.

Deliberate, and the skeleton recommends it: an unbalanced tree is slow, a tree
that loses items is broken. Correctness first.

**What forces it:** nothing before phase 4. Merging needs to free blocks, and
freeing needs the extent tree; implementing it against the bump allocator would
mean writing the hard half twice.

---

## What broke

### Batching waited for a forcing function that did not exist

*Phase 7, 2026-09-16.*

`bitterfs_setattr` was finished, correct, and completely invisible. `chmod`
returned 0, `stat` showed the new mode, `umount` and a fresh `mount` showed the
old one. `bitter-fsck` reported zero problems on the image, which is the part
worth remembering: the filesystem was perfectly consistent and simply did not
contain the change.

`bitterfs_trans_end` commits only when `ref_count >= BITTERFS_DELAYED_REFS -
BITTERFS_DELAYED_MARGIN` -- 3968 of 4096. That threshold is right for what it
was written to do, which is stop a burst of small operations paying for a
commit each. What it quietly assumed is that SOMETHING would eventually force
the issue. Nothing did. A `chmod` adds a handful of refs, `kill_sb` calls
`kill_block_super` and frees the mount's arrays without committing, and there
was no `->sync_fs` because there was no `s_op` table at all -- `sb->s_op` was
the VFS's all-NULL `default_op`, so every `s_op->` call in `fs/` skipped us in
silence.

Fixed by `bitterfs_sync_fs`, which commits whatever is live regardless of the
threshold. One method covers three callers because they all route through
`sync_filesystem()`: `sync(1)`, remount-to-read-only, and unmount --
`generic_shutdown_super` syncs before it calls `->put_super`. The batching then
means what it was always meant to mean: batch within a sync interval, rather
than never commit.

**The lesson worth keeping:** copy-on-write makes a lost write look exactly
like a filesystem that was never written to. The old tree stays intact and
reachable, every checksum verifies, every invariant holds, and `fsck` has
nothing to report -- because there is nothing wrong with the image, only with
what is missing from it. No test that inspects a single mount can see this.
The check that found it was `umount`, `mount`, and read it back.

A threshold that defers work is only half a policy. The other half is whatever
guarantees the deferral ends, and it has to be written down -- or, better,
written.

### Two builds, one object filename

*Phase 5, 2026-09-09.*

`kernel/Kbuild` had carried an UNRESOLVED note since phase 0: Kbuild is awkward
about objects outside the `M=` directory, so `bitterfs-y += ../core/btree.o`
might not work. It was left to be decided at phase 5 with the real files in
hand.

Resolved by moving `M=` up to the repo root rather than reaching down from
`kernel/`. A `Kbuild` at the top names `kernel/*.o` and `core/*.o`, every object
is genuinely beneath `M=`, and no path contains a `..`. The bonus is that
`-I$(src)` and core/Makefile's `-I$(TOP)` become the same directory, so a
`core/` source compiles into the module and into a unit test with no `#ifdef`.

The first build proved the thing phase 5 exists to test. All eight `core/` files
compiled under kernel flags with no errors and no warnings, and the module's
complete list of undefined symbols was `_printk` plus compiler-inserted KASAN,
stack-protector and retpoline entries. `core/` needs nothing from the kernel —
not even `memcpy`. Four phases of `make freestanding` bought exactly what it
claimed to.

**Then the second build broke the first.** Kbuild writes each object beside its
source, which is where `core/Makefile` had been writing its own for four
phases. The kernel's `core/btree.o` is `-mcmodel=kernel`, non-PIE and KASAN
instrumented; archiving it into `libbitter.a` fails the userspace link with
`relocation R_X86_64_32S ... can not be used when making a PIE object`. That
error names the symptom and not the cause, and nothing in it mentions the
kernel build.

The nastier half is the timestamps. Each build leaves an object newer than its
source, so the other build sees it as up to date and never rebuilds. `make` does
not repair it; the damage persists until something is cleaned by hand.

Fixed by giving the userspace build its own `core/build/` directory. Kbuild
cannot be moved off `.o`-beside-`.c` for an external module, so that build is
the one that gives way. `core/libbitter.a` stays where it was, which is the only
path `user/` and `tests/` ever name — the change is contained entirely within
`core/Makefile`.

**The lesson worth keeping:** the probe that validated the layout ran on a
*copy* of the tree, so it could not have found this. Copying the sources tested
whether Kbuild accepted the shape and silently discarded the question of what
the build does to a directory something else already owns. A build experiment
that cannot damage anything also cannot show you the damage.


### A growable array that grew its capacity but not its memory

`fsck_push` raised `wl->cap` when the worklist filled -- `cap <<= 1; cap += 1`
-- and never called `realloc`. `wl->item` stayed NULL, so the capacity said
there was room and the next write went through a null pointer.

Its sibling failed the opposite way: `fsck_refs` returned `-BITTER_ENOMEM` when
full and never grew at all, and `main` hands it a zero-capacity array, so the
very first push would have refused.

Both come from one confusion, and it is worth naming because the codebase now
has arrays of both kinds. In `core/` -- the free-space map, the delayed-ref set
-- a full array IS an error, because `core/` is freestanding and the storage
belongs to a caller. In `user/` it is not: fsck owns its arrays and may
allocate, so full means grow. The two look identical at the point of use and
behave oppositely, so the rule belongs in a comment at every one of them.

The growth itself has a third trap the rewrite had to name: `cap * 2` is 0 when
`cap` is 0, so a zero-initialised array doubles forever without making room.

### Newly allocated blocks were never stamped with the fsid

`bitter_alloc_block` fills every header field a new block needs -- bytenr,
nritems, owner, level, generation -- and had never written `fsid`, because
nothing had ever read it. Arming the fsid check turned that into an immediate
failure: the first block allocated during the first transaction was rejected as
belonging to another filesystem, by the filesystem that had just created it.

The fix is a division, not a line. `core/` cannot know the fsid -- it is in no
structure `core/` holds, and adding one would be a third copy to keep in step
-- so the ENVIRONMENT stamps it, in `alloc_block_buf`, exactly as the
environment stamps the checksum in `put_block`. `core/` fills the fields it can
compute and touches neither of the two that identify the filesystem itself.

Worth noting what this says about the check that found it: a field nothing
reads is not maintained, whatever the code appears to say. The header had
carried a documented `fsid` on every block since phase 1, and half of them were
zero.

### The free map handed out the superblock

The scan builds the free map from the gaps between extent items, and mkfs
recorded none — it wrote three blocks and told the extent tree about zero of
them. So a freshly formatted image reported one free range covering the entire
device, address 0 upward, and the allocator duly began handing out the reserved
prefix, the superblock, and the two trees.

What made it hard to read is where it surfaced. Nothing failed at allocation
time; the writes all succeeded. It appeared as `btree_search` returning
`-BITTER_EUCLEAN` on the 118th insert, from a checker rejecting a block whose
contents had been overwritten by an allocation several calls earlier. The
symptom was a corrupt tree; the cause was a correct allocator obeying a map
that was telling the truth about a filesystem that had not described itself.

The fix was mkfs writing three extent items covering what it lays down —
`[0, 0x11000)` for the reserved region and superblock, one block each for the
two trees. The scan then needs no constants at all, which is the property worth
having: `core/` learns the layout by reading it, not by being told.

The generalisation: derived state is only as good as what it is derived from,
and the failure mode of a wrong derivation is not an error but a plausible
answer.

### The same by-value mistake three times in one phase

Three separate functions took a copy of a structure that had to be updated in
place, and each would have failed silently:

`bitter_find_root` copies `tree_root->next_free` into the root it fills, giving
every tree its own bump cursor and letting two of them hand out one address.

`extent_dec_ref` began `struct bitter_root extent_root = fs_info->extent_root;`
— so the copy-on-write descent's relocation of the extent tree's root died at
`return`, and the filesystem would have kept using the stale root while the new
one sat unreferenced.

`bitter_alloc_block` began `struct bitter_free_extent ext = ext_arr[0];`, so
shrinking the range updated nothing and every allocation would have returned
the same address forever.

Only the first was caught by reasoning; the other two by review. None would
have been caught by the compiler, and the second and third produce no
diagnostic of any kind — just a filesystem that quietly disagrees with itself.

The tell is the same each time: a local whose value is read back by somebody
else later. Where that is true, take the pointer.

### A duplicated field nobody read had rotted

*2026-09-03.*

`btree_search` now cross-checks the generation the parent records for a child
against the child's own header — closing what was, for most of a day, an open
debt in this file. Turning it on immediately failed three tests, and none of the
causes were in the new code.

`bt_build_node` wrote `1` into every key_ptr's generation, with a comment
saying it "mirrors the child's own header.generation, which is 1 everywhere."
The builders memset their headers, so every child was generation 0. The comment
described a convention the fixtures had never followed, and it went unnoticed
because nothing read the field.

`bt_fixture_open` left `root.generation` uninitialised, which happened to be 0
and so happened to work for exactly one insert.

And `btree_split_node` and `btree_split_leaf` both updated `root->bytenr` and
`root->level` when growing a new root without updating `root->generation`. That
one is silent in normal operation: an insert has already CoW'd the root into the
current transaction, so the new root's generation matches by coincidence.
`split_root.c` calls split directly with no preceding CoW, and is the only
reason it surfaced.

The lesson is about the first of the three. A field that is written but never
read has no invariant — only a comment claiming one, which nothing tests and
which drifts. The generation in `bitter_key_ptr` was correct-looking and wrong
for as long as it was unverified. Duplicated state is only as good as the check
that compares the copies.

### The CoW parent branch was untested until a second transaction existed

*2026-09-03.*

`tests/unit/cow.c` was written because mutation-testing showed the existing
suite could not tell whether `btree_cow_block` ran at all. Disabling CoW
entirely, and separately leaving the parent pointing at the old block, both
left every test green.

The reason is worth keeping: fixture blocks are generation 0 and the initial
tree is a single leaf that **is** the root, so the one relocation that ever
happened took the root branch. Every block after it is created by a split,
which allocates through `bitter_alloc_block` and is therefore born owned by the
current transaction. Nothing in a single-transaction test ever meets an older
interior block.

The lesson generalises past CoW: a test that only ever exercises freshly created
state cannot see a bug in code that handles inherited state.

### `bitter_leaf_remove` wrote past the end of the block

*Phase 2, found on first use.*

The cosmetic zero-fill of vacated bytes targeted `victim_offset + move_len -
size` where it meant `victim_offset - move_len`. Because `move_len` is
`victim_offset - lowest`, the wrong expression evaluates to `2*victim_offset -
lowest - size` — on a nearly full leaf, well past 4096. Heap corruption,
reported by valgrind as an invalid 8-byte write at an address 432 bytes
inside an unallocated block.

The code was written during phase 2 and had never been executed: `btree_del_item`
was its first caller. Untested code is not code that probably works; it is code
whose bugs are still in front of you.

### `btree_cow_block` released a buffer the path still held

*2026-09-03, found in review.*

The first draft assigned the new block to a local and released the original,
without writing the new block back into `path->nodes[level]`. The caller would
have gone on using freed memory, and `bitter_path_release` would have put the
same buffer twice.

What hid it was two names for the same block: `buf = buf_temp` *looks* like the
handover, and the line that actually performs the handover was absent. The
function now keeps one name per block, and a mutation that removes
`path->nodes[level] = buf_temp` segfaults `unit/cow` immediately.

### A missing `bitter_path_release` that valgrind did not report

*Phase 2.*

`btree_del_item`'s not-found exit originally returned without releasing the
path. Valgrind reported nothing, because the test driver reused one path across
every call and the next `btree_search` releases on entry — the leak was
overwritten before it could be observed.

`unit/del_item` now uses a dedicated path for that one case and deliberately
does not release it. With that in place the same omission reports 12,384 bytes
definitely lost.

The lesson: a leak test proves nothing if the harness recycles the leaked
object. Reuse in a fixture is not neutral.

---

## Paid

### Nothing writes INODE_REF

*Incurred: phase 7, 2026-09-18.*

`BITTER_INODE_REF` is key type 25, it has two static asserts fixing its sort
position between `INODE_ITEM` and `DIR_ITEM`, and that is the whole of its
existence. There is no `struct bitter_inode_ref`, no size constant, no code,
and neither mkfs nor `->create` writes one.

It is the inverse of a dirent: given an inode, which directories name it and
under what name. btrfs keys it `(inode, INODE_REF, parent_dir)` with a payload
of the `DIR_INDEX` value, the name length, and the name.

**Why it was acceptable:** nothing reads it. `lookup` searches `DIR_ITEM` by
name hash and `readdir` walks `DIR_INDEX`; neither needs the reverse direction,
and `->create` establishes `nlink = 1` without consulting anything.

The usual argument for settling a format field early is that every image
written before it exists will lack the field forever. That does not bite here:
images in this project are disposable, `mkfs` runs before every smoke run, and
nothing outside the repository has ever mounted one.

**What will force it:** `->unlink`, and not as a nicety. Removing a name has to
decrement `nlink`, and doing that correctly means knowing how many names point
at the inode -- which is what the backref set answers. Without it, the
alternatives are trusting the `nlink` already on disk (which is circular: it is
exactly the number being repaired) or scanning the whole FS tree for dirents
that point at this inode. The same item also carries the `DIR_INDEX` value, so
unlink can delete the index entry without a second search.

`link(2)` would add a second one; `fsck` would use them to check that every
dirent has a matching backref and that `nlink` equals the count. All three
arrive together, and the cost of arriving late is one `mkfs`.

**Paid: 2026-09-18**, and earlier than the entry predicted -- not by `->unlink`
needing it, but by `->create` being the only thing that ever writes one. A file
created before the field existed would carry no backref for its whole life, so
unlink would have had to cope with two kinds of inode. Cheaper to write them
from the first file than to teach the reader that some are missing.

`struct bitter_inode_ref` is a 10-byte header -- the `DIR_INDEX` this name was
given, then `name_len` -- followed by the name, keyed
`(inode, INODE_REF, parent)`. `bitterfs_add_inode_ref` writes one in
`->create`, and mkfs writes two: the root directory's, which names itself and
carries "..", and the initial file's.

Verified by reading the leaf back: seven items after mkfs where there were
five, sorted correctly with each backref between its inode item and the entries
that follow, and inode 258 -- the first file this filesystem ever created --
carrying one.

The hard-link half is NOT paid and now has its own limitation: the key is
unique per (inode, parent), so two links to one file in one directory cannot
both exist. `->link` meets that, not `->create`, which only ever runs on a name
lookup already proved absent.

### The filesystem mutex is taken in the wrong place

*Incurred: phase 5, 2026-09-11.*

`bitterfs_iget` calls `mutex_lock(&mnt->lock)` itself, around its `btree_search`
and the path release. That is correct today because `fill_super` is its only
caller and holds nothing.

It stops being correct at phase 6. `lookup` will take the mutex, search the
directory for a name, and call `bitterfs_iget` on the inode number it finds —
which re-acquires a mutex the same task already holds. `struct mutex` is not
recursive, so that is a self-deadlock: the task sleeps uninterruptibly on a lock
it owns, and the hung-task splat names a waiter rather than a cause.

The kernel's convention is the other way round — acquire at the VFS entry
points, and have helpers assert rather than acquire. btrfs is full of
`lockdep_assert_held(&fs_info->commit_root_sem)` for exactly this. The fix is
one line in each direction: `mutex_lock` moves out to the callers, and
`bitterfs_iget` gains `lockdep_assert_held(&mnt->lock)`, which with
`CONFIG_PROVE_LOCKING=y` genuinely fires on a caller that forgot.

Acceptable only because there is exactly one caller and it is `fill_super`.

**What forces it:** the second caller, which is phase 6's `lookup`. Do it as the
first change of that phase rather than after the first hang.

**Paid: 2026-09-11**, before the deadlock could happen rather than after.
`bitterfs_iget` now opens with `lockdep_assert_held(&mnt->lock)` and acquires
nothing; `fill_super` takes the mutex around its call.

`fill_super` does not need the lock — the mount is unreachable until `s_root` is
set, so nothing can contend. It takes it anyway, so the precondition holds
uniformly and the assertion has no documented exception. A precondition with one
exception is a precondition nobody checks.

The one thing to revisit: `iget_failed` runs inside the caller's lock now, and it
ends in an `iput` that can reach `evict_inode`. There is no `evict_inode` yet, so
the generic path runs and nothing re-enters the filesystem. When phase 7 adds
one, that call has to move back outside.

### `next_free` is copied per tree, not shared

*Incurred: phase 4, 2026-09-04.*

`bitter_find_root` fills `out->next_free` by copying `tree_root->next_free`.
Because the field is a `bt_u64` inside `struct bitter_root`, each tree then
carries its own independent bump cursor. Allocating from one does not advance
the other, so two trees hand out the same address, and nothing at runtime
notices — `header.owner` would catch it at fsck, and only there.

The second failure outlives the mount. `trans_commit` persists exactly one
root's `next_free` into `super.bytes_used`, so any allocation made through a
different root sits above the recorded high-water mark and is handed out again
on the next open.

Accepted because the constraint it implies is still true: only one tree
allocates per transaction, and until `bitter_update_root` exists there is only
ever one tree in play. The real diagnosis is that `next_free` is
filesystem-wide state living in per-tree state, which was invisible while "one
tree" and "one filesystem" meant the same thing.

**What forced it:** the free-space map, sooner than expected. `bitter_update_root`
was going to be the first moment two roots were live at once, but the allocator
was rewritten before that mattered.

**Paid: 2026-09-06.** `bitter_alloc_block` takes from `fs_info->free`, reached
through a back-pointer in `struct bitter_root`. One map per filesystem, so two
trees can no longer be handed the same address — the copy that caused this
still happens in `bitter_find_root`, but nothing allocates from it any more.

The field itself is vestigial and still moves: `bitter_read_super` restores it
from `super.bytes_used`, `trans_commit` writes it back, `bitter_find_root`
copies it, and the fixtures keep it in step so `unit/commit` can still check it
round-trips. Deleting it is a separate cleanup, and it will take
`super.bytes_used` with it.

### Allocated blocks are not recorded, so a rescan gives them away

*Incurred: phase 4, 2026-09-07.*

`bitter_alloc_block` takes an address out of the free-space map and nothing
puts a matching item in the extent tree. The map is derived state, thrown away
at unmount and rebuilt by `extent_build_free_map`, so any block handed out but
not recorded is free again the next time the scan runs — and will be handed to
something else while the first owner is still using it.

Measured rather than reasoned about: a run that inserted 500 extent items into
a 64 MiB image reported 64983040 bytes free afterwards, against a device of
67108864. The difference is exactly `0x13000 + 500 × 4096` — mkfs's blocks and
the 500 recorded extents, and nothing at all for the ten-odd blocks the extent
tree allocated for its own nodes and leaves.

Not dangerous today only because nothing remounts mid-workload. It becomes
data loss the moment a filesystem is opened twice.

**What forced it:** the delayed-ref set in `struct bitter_trans`, wired at both
ends. `bitter_alloc_block` cannot call
`extent_inc_ref` directly — recording a reference modifies the extent tree,
which copy-on-writes it, which allocates, which needs another reference
recorded. Deferring is what breaks that cycle, because the in-memory map has
already been updated and nothing else can be handed the same address before
commit applies the set.

**Paid: 2026-09-07.** `bitter_alloc_block` records a `+1` for every block it
hands out; `trans_commit` drains the set through `extent_apply_delayed_refs`
before writeback, then records the extent tree's moved root and repeats until
both settle.

Measured the same way it was found. 300 inserts into a fresh 64 MiB image,
committed, then remounted from scratch: 102400 bytes accounted as allocated,
which is mkfs's `0x13000` plus exactly the six blocks the transaction used, and
all 300 keys readable from a filesystem the process never built.

### fsid is not verified on read

*Incurred: phase 3, 2026-09-03.*

`user_env_read_block` performs two of the three self-identifying checks the
format promises — the crc32c, and `header.bytenr` matching the address the
block was read from. The third, `header.fsid` against the filesystem's own, is
not done.

It cannot be. `struct env_user` holds an fd and a path, and the test fixtures
never write a superblock at all — `bt_fixture_open` truncates a file and builds
a tree in it. There is nothing to compare against.

**What forced it:** fsck, which needed the check a phase earlier than
expected. It walks every block in the filesystem, so it is the first thing that
would have read a stale block from a previous mkfs and believed it.

**Paid: 2026-09-08.** `struct env_user` carries the fsid and a `have_fsid`
gate; `bitter_env_user_set_fsid` arms it, and `bitter_fs_info_init` calls that
as soon as the superblock has been validated. Every read for the life of a
mount is checked, ordered after csum and bytenr -- those two say the bytes are
intact and where they claim to be, and only then is "whose are they?"
meaningful. mkfs and the unit fixtures never arm it, so they are unaffected.

Verified by planting a block with a foreign fsid and a correctly re-stamped
crc32c at 0x11000: csum and bytenr both pass, and the read is refused. That is
the failure mode the entry was written about, caught for the first time.

