/*
 * Assertions for core/, which cannot use <assert.h>.
 *
 * That header is not freestanding, so a file compiled into the kernel module
 * cannot include it -- the same bind as <string.h> and <errno.h>.  The way
 * out is a LINK-time seam rather than another entry in bitter_env_ops:
 * core/ declares bitter_assert_fail() and the consumer defines it.
 *
 *     user/util.c     prints and abort()s, so a test dies where the bug is
 *     kernel/         panic() or BUG(), when phase 5 needs one
 *
 * Lighter than a function pointer because assertions are for invariants that
 * must hold in every environment; there is nothing per-filesystem to configure.
 *
 * Use for what MUST be true given the code is correct -- a slot within
 * nritems, a level under BITTER_MAX_LEVEL, free space that does not exceed the
 * block.  Not for anything derived from disk contents: a corrupt block is an
 * expected condition and must return an error, not abort.  The test is whose
 * fault a failure would be.  Yours: assert.  The disk's: return -EIO.
 */
#ifndef BITTER_ASSERT_H
#define BITTER_ASSERT_H

/*
 * Never returns.  A kernel implementation that merely warned and continued
 * would be undefined behaviour here, and would also mean the code after a
 * failed assertion runs with the invariant broken -- which is the situation
 * the assertion existed to prevent.
 */
_Noreturn void bitter_assert_fail(const char *expr, const char *file, int line);

#ifdef BITTER_DEBUG

#define BITTER_ASSERT(cond)                                            \
  do {                                                                 \
    if (!(cond)) {                                                     \
      bitter_assert_fail(#cond, __FILE__, __LINE__);                   \
    }                                                                  \
  } while (0)

#else

/*
 * sizeof rather than ((void)0): it type-checks the expression without
 * evaluating it, and still counts as a USE of anything mentioned -- so a
 * variable that appears only inside assertions does not become
 * -Wunused-variable when they are compiled out.
 *
 * The ?: keeps it valid for pointer and float conditions too, where plain
 * sizeof(cond) would be fine but reads as if the size mattered.
 */
#define BITTER_ASSERT(cond) ((void)sizeof((cond) ? 1 : 0))

#endif

#endif
