/*
 * File contents: the key arithmetic for EXTENT_DATA items.
 *
 * Companion to core/dir.c, and the same split -- this side knows keys and
 * bytes, kernel/file.c knows folios and buffer heads.
 *
 * Deliberately NOT in core/extent.c, despite the name.  That file owns the
 * EXTENT TREE, whose items are keyed by device address and count references.
 * These are keyed by FILE offset and describe where one file's bytes live.
 * Two different item types, two different trees, one unfortunate word.
 */
#ifndef BITTER_FILE_H
#define BITTER_FILE_H

#include "format/bitterfs_format.h"
#include "core/items.h"

void bitter_file_extent_key(struct bitter_key_cpu *key, bt_u64 ino,
        bt_u64 file_offset);

#endif
