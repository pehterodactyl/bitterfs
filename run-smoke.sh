#!/bin/sh
# Rebuild, reformat, boot, report.  The reformat is the point: guest.sh assumes
# a pristine image, and running the VM twice without one produces failures that
# look like bugs and are not.
set -e
cd "$(dirname "$0")"
make -C kernel >/dev/null
./user/mkfs.bitterfs qemu/bitter.img >/dev/null
cd qemu && exec ./run.sh --smoke "$@"
