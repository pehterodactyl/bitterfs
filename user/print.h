/*
 * Output formatting shared by the tools.  Separate from util.c, which is about
 * reporting failures -- these are for reporting success.
 *
 * Every field printer takes a name and a value and emits one aligned row, so
 * the caller decides WHAT to show and never argues with the compiler about
 * column widths.
 */
#ifndef BITTER_PRINT_H
#define BITTER_PRINT_H

#include <stddef.h>
#include "format/bitterfs_format.h"

/* 16 bytes -> "8-4-4-4-12".  `out` must hold PRINT_UUID_LEN bytes. */
#define PRINT_UUID_LEN 37
void format_uuid(char *out, const bt_u8 *fsid);

/* 1073741824 -> "1.0 GiB" */
void human(char *buf, size_t n, bt_u64 bytes);

/* A blank line then an underlined title. */
void print_section(const char *title);

/* One aligned "name  value" row.  print_size adds a human-readable suffix;
 * print_hex64 prints 0x-prefixed; print_bytes prints n bytes as hex. */
void print_str(const char *name, const char *value);
void print_u64(const char *name, bt_u64 value);
void print_u32(const char *name, bt_u32 value);
void print_hex64(const char *name, bt_u64 value);
void print_size(const char *name, bt_u64 bytes);
void print_uuid(const char *name, const bt_u8 *fsid);
void print_bytes(const char *name, const bt_u8 *b, size_t n);

/*
 * One validation row: "  what ......... ok" or "FAIL".  Returns `pass`
 * unchanged so a caller can accumulate:
 *
 *     bad |= !print_check("csum verifies", computed == stored);
 */
int print_check(const char *what, int pass);

/* xxd-style: offset, 16 bytes of hex, ASCII gutter.  `base` is the address
 * shown in the left column, so a caller dumping a block at 0x11000 passes
 * that rather than 0. */
void hexdump(const void *buf, size_t len, bt_u64 base);

#endif
