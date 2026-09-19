# ===========================================================================
# bitterfs — unit tests for core/
# ===========================================================================
#
#   make              build and run every test
#   make list         show which tests exist
#   make unit/foo     build one test without running it
#   make run-foo      build and run one test
#   make valgrind     run every test under valgrind
#   make clean
#
# Tier 1 of docs/skeleton.tex: core/ exercised in userspace, which is the whole
# point of the seam.  A B-tree bug found here costs thirty seconds; the same
# bug found by insmod costs a panic and a reboot.
# ===========================================================================

TOP  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
CORE := $(TOP)/core
USER := $(TOP)/user
LIB  := $(CORE)/libbitter.a

# -I$(TOP) for the "core/..." and "format/..." includes, -I$(USER) for
# env_user.h and util.h, -I. for test.h.
INCLUDES := -I$(TOP) -I$(USER) -I.

# Assertions ON: a test build is exactly where BITTER_ASSERT should abort.
# -g always, because the first thing you do with a failing test is run it under
# gdb, and rebuilding to add symbols loses the failing seed.
STD      := -std=c11
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wvla \
            -Wstrict-prototypes -Wold-style-definition \
            -Wpointer-arith -Wwrite-strings -Wformat=2
DEFINES  := -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE -DBITTER_DEBUG

CC     ?= cc
CFLAGS ?= -O1 -g

# -O1 rather than -O2 by default: valgrind reports are far more readable, and
# nothing here is performance sensitive.  Override for a timing run.
#
# SANITIZE=1 adds ASan and UBSan.  Not on by default because they conflict with
# valgrind -- use one or the other, and `make clean` between them, since the
# objects are not interchangeable.
ifeq ($(SANITIZE),1)
  SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer \
              -fno-sanitize-recover=undefined
endif

ALL_CFLAGS = $(STD) $(WARNINGS) $(DEFINES) $(CFLAGS) $(SANFLAGS) -MMD -MP $(INCLUDES)
LDFLAGS   += $(SANFLAGS)

# Every .c in unit/ is one test with its own main().  Adding a test means
# adding a file; there is nothing to register.
SRCS  := $(sort $(wildcard unit/*.c))
TESTS := $(SRCS:.c=)
DEPS  := $(SRCS:.c=.d) test.d

# The env implementation, die/print, and the transaction lifecycle -- none of
# which core/ contains.  trans.o is what lets a test open a real image, commit,
# and reopen it; built by user/Makefile, linked here.
SUPPORT := $(USER)/env_user.o $(USER)/util.o $(USER)/print.o $(USER)/trans.o \
           $(USER)/format.o

# run-% is deliberately NOT .PHONY: GNU make refuses to apply a pattern rule to
# a target it has been told is phony, so declaring these would silently break
# `make run-NAME`.  They can never collide with a real file anyway.
.PHONY: all list clean valgrind core user help

.DEFAULT_GOAL := all

help:
	@echo 'bitterfs unit test targets:'
	@echo '  make              build and run every test'
	@echo '  make list         show which tests exist'
	@echo '  make run-NAME     build and run one test, e.g. run-leaf'
	@echo '  make valgrind     run every test under valgrind'
	@echo '  make SANITIZE=1   rebuild with ASan + UBSan (make clean first)'

list:
	@$(if $(TESTS),printf '  %s\n' $(notdir $(TESTS)),echo '  (no tests yet -- add unit/NAME.c)')

# core/ and user/ are rebuilt first: editing core/btree.c must rebuild the
# tests that exercise it, and only those Makefiles know their dependencies.
core:
	@$(MAKE) --no-print-directory -C $(CORE)
user:
	@$(MAKE) --no-print-directory -C $(USER)

$(LIB) $(SUPPORT): core user
	@:

test.o: test.c test.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

unit/%.o: unit/%.c test.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

unit/%: unit/%.o test.o $(LIB) $(SUPPORT)
	$(CC) $(ALL_CFLAGS) -o $@ $< test.o $(SUPPORT) $(LIB) $(LDFLAGS)

# Runs everything and reports at the end rather than stopping at the first
# failure -- when a change breaks three tests, you want to see all three.
all: $(TESTS)
	@if [ -z "$(TESTS)" ]; then \
	  echo 'No tests yet.  Add unit/NAME.c with a main() and it is picked up.'; \
	  exit 0; \
	fi; \
	fail=0; \
	for t in $(TESTS); do \
	  echo "=== $$t ==="; \
	  ./$$t || fail=1; \
	done; \
	echo; \
	if [ $$fail -eq 0 ]; then echo 'ALL TESTS PASSED'; \
	else echo 'SOME TESTS FAILED'; exit 1; fi

# --error-exitcode makes a leak or an invalid access fail the run, not just
# print.  A leaked bitter_buf is the failure this tier exists to catch, since
# every path->nodes[i] must reach put_block on every exit including the errors.
valgrind: $(TESTS)
	@fail=0; \
	for t in $(TESTS); do \
	  echo "=== $$t (valgrind) ==="; \
	  valgrind -q --leak-check=full --errors-for-leak-kinds=definite \
	           --error-exitcode=9 ./$$t || fail=1; \
	done; \
	echo; \
	if [ $$fail -eq 0 ]; then echo 'ALL TESTS PASSED UNDER VALGRIND'; \
	else echo 'FAILURES UNDER VALGRIND'; exit 1; fi

run-%: unit/%
	./$<

clean:
	rm -f $(TESTS) unit/*.o unit/*.d test.o test.d

-include $(DEPS)
