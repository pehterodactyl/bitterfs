/*
 * crc32c (Castagnoli) — the checksum every bitterfs block carries.
 *
 * NOT the CRC-32 of zip, gzip and PNG: different polynomial, different
 * answers.  cksum and zlib will disagree with a correct implementation.
 *
 * Pinned exactly, in Williams' parameter model (docs/skeleton.tex, Reading
 * list):
 *     width=32   poly=0x1edc6f41    init=0xffffffff
 *     refin=true refout=true        xorout=0xffffffff
 *     check=0xe3069283              name="CRC-32/ISCSI"
 *
 * `check` is the crc of "123456789", and it is the test that matters: a wrong
 * table, polynomial, shift direction or loop all still produce plausible
 * 32-bit numbers.
 */
#ifndef BITTER_CRC
#define BITTER_CRC

/*
 * bt_crc32c is a RAW CORE, like the kernel's __crc32c_le: it applies neither
 * the initial value nor the final XOR, so callers own both.
 *
 *     bt_u32 c = bt_crc32c(BT_CRC32C_INIT, buf, len) ^ BT_CRC32C_INIT;
 *
 * Named so it cannot be mistyped — 0xFFFFFFF is a wrong checksum that looks
 * reasonable.  The two exist so that leading and trailing zero bytes cannot
 * pass undetected; neither is part of the mathematics.
 */
#define BT_CRC32C_INIT 0xFFFFFFFFu

#include "format/bitterfs_format.h"
  /*
   * Folds `len` bytes of `data` into `crc` and returns the new running value.
   * Composable, so a header and its payload need no copy to be contiguous:
   *
   *     c = BT_CRC32C_INIT;
   *     c = bt_crc32c(c, header, sizeof *header);
   *     c = bt_crc32c(c, payload, len);
   *     c ^= BT_CRC32C_INIT;
   *
   * Returns bt_u32 — a host-order number with no byte order until
   * bt_put_le32 writes it.  Not static inline, unlike the endian accessors:
   * a loop and a 1KB table gain nothing from being inlined per call site.
   */
  bt_u32 bt_crc32c(bt_u32 crc, const void* data, bt_u32 len);  

  /*
   * The whole-block incantation in one place, so the initial value and the
   * final XOR cannot go missing from one caller.  Only `len` legitimately
   * differs: a tree block checksums its whole 4096, the superblock stops at
   * 512 because that is the sector the drive writes atomically.
   *
   * Skips the leading BITTER_CSUM_SIZE bytes, because a field cannot be part
   * of its own input.
   *
   * Lives in core/ rather than user/ so that the kernel module and the
   * userspace tools cannot disagree about what a valid block is -- a
   * disagreement whose symptom is bitter-fsck pronouncing an image clean that
   * the kernel then refuses to mount.
   */
  bt_u32 bt_block_csum(const void* buf, bt_u32 len);

#endif
