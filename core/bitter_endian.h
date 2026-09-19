/*
 * bitterfs byte-order accessors.
 *
 * Everything on disk is little-endian, and this is the only file that knows
 * it — a big-endian port changes nothing anywhere else.  In core/ rather than
 * format/ because these are functions and format/ is declarations only.
 *
 * Header-only, static inline, no .c.  A B-tree search binary-searches ~160
 * items per node and every key comparison reads three fields, so a call per
 * field read would be absurd — and the compiler cannot inline what it cannot
 * see.  `static` is not optional: #include is textual paste, so each
 * translation unit gets a private copy, and internal linkage is what stops
 * those copies colliding at link time.
 *
 * The byte-wise bodies are correct on a big-endian host too, which is what
 * keeps core/ free of #ifdefs — the property that lets these sources compile
 * into both the kernel module and the userspace tools.
 *
 * Design notes and the measurements behind them: docs/notes.tex, "Accessors
 * the optimiser can see through".
 */
#ifndef BITTER_ENDIAN_H
#define BITTER_ENDIAN_H
#include "format/bitterfs_format.h"

/*
 * `const void *`, not `const bt_le64 *`: call sites pass the address of a
 * packed member, which warns under -Waddress-of-packed-member through a typed
 * pointer but not through void *.  And bt_le64 and bt_u64 are the same
 * typedef, so a typed parameter would look like it checked something.
 *
 * The little-endian side is always behind a pointer, the host side always a
 * plain value — byte order is a property of a layout in memory, not of a
 * value in a register.
 *
 * Bytes rather than *(const bt_u64 *)p: the structures are packed so many of
 * these addresses are unaligned, and reading through unsigned char * is the
 * one case strict aliasing explicitly permits.  The VALUE is cast, never the
 * pointer, and the cast sits inside the shift — promotion makes
 * (bt_u64)(b[7] << 56) undefined and silently zero.
 *
 * Written longhand because gcc's byte-combining idiom recogniser matches a
 * straight-line expression, not a loop.  This is what the loop would have been:
 *
 *     bt_u64 num = 0;
 *     for (int i = 0; i < 8; i++) num += (bt_u64)indexer[i] << 8*i;
 *     return num;
 *
 * Equally correct, and ~40 instructions at -O2 where this is one movq — even
 * after -funroll-loops, because by then the shift counts come from an
 * induction variable rather than being constants in the source.
 */
static inline bt_u64 bt_get_le64(const void* p) {
  const unsigned char* indexer = (unsigned char*) p;
  return (bt_u64)indexer[0] | ((bt_u64)indexer[1] << 8) |
         ((bt_u64)indexer[2] << 16) | ((bt_u64)indexer[3] << 24) |
         ((bt_u64)indexer[4] << 32) | ((bt_u64)indexer[5] << 40) |
         ((bt_u64)indexer[6] << 48) | ((bt_u64)indexer[7] << 56);
 
}


/*
 * The mirror, with the cast at the other end: `num` is already 64-bit, so the
 * shift is safe at full width and the cast narrows on the way out.  Getter
 * casts first, putter casts last — if both read the same way, one is wrong.
 * The local is non-const here because it must write through it.
 *
 * Returns p, following memcpy/memset, so consecutive fields can be chained.
 * Free: the copy vanishes wherever the result is unused.
 */
static inline void* bt_put_le64(void* p, bt_u64 num) {
  unsigned char* b = p;
  b[7] = (unsigned char)(num >> 56);
  b[6] = (unsigned char)(num >> 48);
  b[5] = (unsigned char)(num >> 40);
  b[4] = (unsigned char)(num >> 32);
  b[3] = (unsigned char)(num >> 24);
  b[2] = (unsigned char)(num >> 16);
  b[1] = (unsigned char)(num >> 8);
  b[0] = (unsigned char)(num >> 0);
  return p;
}


/* Same shape at 32 bits.  The return type must match the field width: a
 * bt_u64 here would narrow on assignment and break %u under -Wformat=2. */
static inline bt_u32 bt_get_le32(const void* p) {
  const unsigned char* indexer = (unsigned char*) p;
  return (bt_u32)indexer[0] | ((bt_u32)indexer[1] << 8) |
         ((bt_u32)indexer[2] << 16) | ((bt_u32)indexer[3] << 24);
}

static inline void* bt_put_le32(void* p, bt_u32 num) {
  unsigned char* b = p;
  b[3] = (unsigned char)(num >> 24);
  b[2] = (unsigned char)(num >> 16);
  b[1] = (unsigned char)(num >> 8);
  b[0] = (unsigned char)(num >> 0);
  return p;
}


/* At 16 bits the (bt_u16) casts are no-ops — integer promotion widens to int
 * before the shift regardless.  Kept for symmetry; correctness here comes from
 * int being 32 bits. */
static inline bt_u16 bt_get_le16(const void* p) {
  const unsigned char* indexer = (unsigned char*) p;
  return (bt_u16)indexer[0] | ((bt_u16)indexer[1] << 8);
}

static inline void* bt_put_le16(void* p, bt_u16 num) {
  unsigned char* b = p;
  b[1] = (unsigned char)(num >> 8);
  b[0] = (unsigned char)(num >> 0);
  return p;
}

#endif
