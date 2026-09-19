/*
 * The few libc functions core/ is allowed to call.
 *
 * <string.h> is not a freestanding header, so a file compiled into the kernel
 * module cannot include it -- the same bind as <assert.h> and <errno.h>.  But
 * unlike those, the FUNCTIONS exist in both worlds: libc provides them, and
 * the kernel provides its own and exports them to modules
 * (EXPORT_SYMBOL(memmove) in lib/string.c, and the arch versions).
 *
 * So the problem is naming them, not reaching them -- and a declaration is all
 * that is missing.  Declaring them here rather than reimplementing avoids
 * writing a correctly-overlapping copy by hand, which is a well-known way to
 * get something subtly wrong, and keeps the architecture-tuned versions in
 * both environments.
 *
 * __SIZE_TYPE__ because size_t comes from <stddef.h>, which is equally out of
 * reach.  Same compiler-macro trick as __UINT64_TYPE__ in format/.
 *
 * Nothing else from libc belongs here.  If core/ needs something with no
 * kernel counterpart, that is a signal it belongs behind bitter_env_ops
 * instead.
 */
#ifndef BITTER_STRING_H
#define BITTER_STRING_H

void *memmove(void *dst, const void *src, __SIZE_TYPE__ n);

/* Non-overlapping only.  Use memmove if you are not certain. */
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n);

/* Zeroing a fresh block, or a region a delete has vacated. */
void *memset(void *dst, int c, __SIZE_TYPE__ n);

/*
 * Compares n bytes.  Needed by core/ from phase 7, when the superblock's magic
 * check moved here with trans_commit -- a byte array with no byte order, so
 * memcmp rather than an accessor, for the same reason format_super uses memcpy
 * for magic and fsid.
 */
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

#endif
