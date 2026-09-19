# tools/

Build-time helpers: programs that **generate sources**, and are never shipped
or linked into anything.

This directory exists so that `core/` can keep one absolute invariant — *every
file in `core/` compiles freestanding* — which is what `make -C core
freestanding` asserts. That assertion is only worth making if it has no
exceptions, so hosted programs live here instead.

Not to be confused with `user/`, which holds tools that are *installed*
(`mkfs.bitterfs`, `bitter-dump`, `bitter-fsck`).

| file | produces | run it with |
|---|---|---|
| `gen_crc32c_table.c` | `core/crc32c_table.h` | `make -C core table` |

The generated files are **committed**, not regenerated on every build: the
crc32c polynomial is fixed by the on-disk format and cannot change without
invalidating every existing image, so a build that silently rewrote a tracked
source file would be pure downside.

The generators are kept in-tree for the same reason the kernel keeps
`lib/gen_crc32table.c`: a 256-entry table with no generator beside it is 256
numbers nobody can verify or reproduce.
