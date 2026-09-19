// SPDX-License-Identifier: GPL-2.0
/*
 * bitterfs — what happens when an invariant in core/ turns out to be false.
 *
 * core/bitter_assert.h declares bitter_assert_fail() and leaves each consumer
 * to define it: user/util.c prints to stderr and abort()s, and this is the
 * kernel's answer.  A link-time seam rather than an entry in bitter_env_ops,
 * because there is nothing per-filesystem to configure.
 *
 * No header of its own.  That declaration is the entire interface, which is
 * why this .c has no matching .h when everything else in the tree pairs.
 */

/* Before any kernel header: printk.h expands pr_*() using whatever pr_fmt() is
 * in scope at that point.  See the long note in hello.c. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bug.h>
#include <linux/printk.h>

/* For the declaration, so it is checked against the definition below. */
#include "core/bitter_assert.h"

/*
 * Report which invariant broke, then stop the task.
 *
 * Reached only through BITTER_ASSERT(), which supplies the stringified
 * condition, __FILE__ and __LINE__.  Assertions cover what must be true if the
 * code is correct; a corrupt block is the disk's fault and returns -EUCLEAN
 * instead, so arriving here always means a bug in core/ and never bad input.
 */
_Noreturn void bitter_assert_fail(const char *expr, const char *file, int line)
{
	/*
	 * BUG() prints a file and line of its own, but they come from
	 * __bug_table and name the site of the BUG() macro — this file, every
	 * time, whichever invariant actually failed.  These three arguments are
	 * the only record of where the real one was, and nothing prints them
	 * for us.
	 *
	 * pr_emerg because this must be legible in the output BUG() is about to
	 * produce, and because there is no later opportunity.
	 */
	pr_emerg("assertion failed: %s at %s:%d\n", expr, file, line);

	/*
	 * ud2 planted inline: the trap handler dumps registers and the stack —
	 * the useful half, since that names the core/ call chain that got here.
	 * Then it kills the task.
	 *
	 * Task-fatal rather than panic() was a deliberate choice; the cost is
	 * that nothing unwinds, so the filesystem-wide mutex is never released
	 * and later access to that mount hangs.  docs/LOG.md has the reasoning.
	 *
	 * This is also what satisfies _Noreturn: BUG() ends in
	 * __builtin_unreachable(), so the compiler agrees control stops here.
	 */
	BUG();
}
