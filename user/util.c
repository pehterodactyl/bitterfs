/*
 * Error reporting shared by mkfs.bitterfs, bitter-dump and bitter-fsck.
 *
 * Not in core/: die() calls exit(), which is right for a one-shot program and
 * wrong for anything the kernel module links, where every failure has to be a
 * return value a caller can propagate.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "util.h"
#include "core/bitter_assert.h"
#include "core/bitter_crc32c.h"

/* Never NULL, so die() can print it with no check — including a die() that
 * fires before set_progname runs. */
static const char* prog_name = "bitterfs";

/*
 * argv[0] with the directory stripped, so `./user/mkfs.bitterfs` reports as
 * `mkfs.bitterfs`.  strrchr returns a pointer AT the slash, hence the + 1.
 *
 * Not basename(): there are two incompatible versions, and the POSIX one may
 * modify its argument.  Also no copy — argv lives for the whole process.
 */
void set_progname(const char* argv0) {
  const char* dir_name = strrchr(argv0, '/');
  if (dir_name) {
    prog_name = dir_name + 1;
  }
}

const char* get_progname(void) {
  return prog_name;
}

/*
 * Three calls rather than one because vfprintf consumes the whole va_list, so
 * the prefix and the newline cannot ride along inside it.  Owning the newline
 * here means no call site has to remember one.
 *
 * The printf format attribute is in util.h, where callers can see it — that is
 * what gets die("%s", 42) caught at compile time.
 */
_Noreturn void die(const char* fmt, ...) {
  fprintf(stderr, "%s: ", prog_name);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

/*
 * As die(), plus ": " and the errno string -- giving the conventional
 * "program: context: cause" shape:
 *
 *     mkfs.bitterfs: open /dev/sbd1: No such file or directory
 *
 * errno is captured on the FIRST line, before any output.  It is never cleared
 * on success and fprintf may set it even when it works, so reading it later
 * can report an error unrelated to the failure being described.
 *
 * Use this only where a syscall actually failed.  For "the label is too long"
 * errno holds whatever some earlier call left behind, and die() is correct.
 */
_Noreturn void die_errno(const char* fmt, ...) {
  int err =errno;
  va_list args;
  fprintf(stderr, "%s: ", prog_name);
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr,": %s\n", strerror(err));
  exit(EXIT_FAILURE);
}

/*
 * The userspace half of core/'s assertion seam (core/bitter_assert.h).
 *
 * abort() rather than exit(): it raises SIGABRT, so a test under gdb stops at
 * the failure with the stack intact, and valgrind prints the trace.  exit()
 * would unwind quietly and lose exactly the information you want.
 */
_Noreturn void bitter_assert_fail(const char* expr, const char* file, int line) {
  fprintf(stderr, "%s: assertion failed: %s at %s:%d\n",
          prog_name, expr, file, line);
  abort();
}
