#ifndef BITTER_UTIL_H
#define BITTER_UTIL_H

#include "format/bitterfs_format.h"

/* bt_block_csum moved to core/bitter_crc32c.h at phase 5: the kernel needs
 * it too, and two definitions of a checksum is two opinions about what a
 * valid block is. */

  void set_progname(const char *argv0);

  const char* get_progname(void);

  __attribute__((format(printf, 1, 2)))
  _Noreturn void die(const char* fmt, ...);

  __attribute__((format(printf, 1, 2)))
  _Noreturn void die_errno(const char* fmt, ...);


#endif
