# w98link - compressed file/command link between Linux and Windows 98
#
# make              build the Linux client into build/bin/
# make debug asan   the other Linux build variants
# make win98        cross-build the Win98 agent with Open Watcom
# make test         build and run the cmocka suite
# make test-junit   the same suite, with JUnit XML under build/junit/
# make coverage     run the suite under gcov and enforce the line gate
# make analyze      cppcheck, gcc -fanalyzer and the stack-usage check
# make tidy         clang-tidy over the sources
# make clean        remove build/
#
# Build artifacts live under build/, one subdirectory per variant, with the
# ready-to-run binaries (both the client and W98AGENT.EXE) in build/bin/.

PYTHON ?= python3

# `CC ?= gcc` would never take effect: make predefines CC as `cc`, so ?= sees
# it already set. $(origin) separates make's default from a real override.
ifeq ($(origin CC),default)
  CC := gcc
endif

VERSION  := $(shell cat release_version)
BUILD_ID := $(shell date -u +%Y%m%d.%H%M)

SRC_DIR   := src
INC_DIR   := include
TEST_DIR  := tests
BUILD_DIR := build
BIN_DIR   := $(BUILD_DIR)/bin

# The protocol core is shared by both ends, so it has to compile under gcc and
# under Open Watcom's C99 mode. Platform code lives beside it, one directory
# per side, and never includes the other side's headers.
CORE_SOURCES   := $(wildcard $(SRC_DIR)/core/*.c)
CLIENT_SOURCES := $(wildcard $(SRC_DIR)/client/*.c) $(wildcard $(SRC_DIR)/posix/*.c)
AGENT_SOURCES  := $(wildcard $(SRC_DIR)/agent/*.c) $(wildcard $(SRC_DIR)/win32/*.c)

# Vendored zlib: built once with its own flags into an archive, never subject
# to this project's warnings, analysis or coverage (third_party/zlib/VENDORED.md).
ZLIB_DIR     := third_party/zlib
ZLIB_SOURCES := $(addprefix $(ZLIB_DIR)/,adler32.c crc32.c deflate.c inffast.c \
                inflate.c inftrees.c trees.c zutil.c)
ZLIB_LIB     := $(BUILD_DIR)/zlib/libz.a

# Tests link the core and the POSIX platform layer, never the client's main().
LIB_SOURCES    := $(CORE_SOURCES) $(wildcard $(SRC_DIR)/posix/*.c)
NATIVE_SOURCES := $(CORE_SOURCES) $(CLIENT_SOURCES)

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wunused-function \
            -Wunused-variable -Wunused-parameter -Wunused-result \
            -Wdouble-promotion -Wformat=2 -Wformat-truncation \
            -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
            -Wcast-align -Wcast-qual -Wnull-dereference -Wconversion \
            -Wsign-conversion

HARDENING := -fstack-protector-strong -fstack-clash-protection

# -isystem, so zlib.h is held to system-header rules rather than ours.
CPPFLAGS_COMMON := -I$(INC_DIR) -isystem $(ZLIB_DIR) -D_GNU_SOURCE \
                   -DW98LINK_VERSION='"$(VERSION)_$(BUILD_ID).$(VARIANT)"'
CFLAGS_COMMON   := -std=c11 $(WARNINGS) $(HARDENING) -Werror

RELEASE_FLAGS  := -O3 -flto -DNDEBUG
DEBUG_FLAGS    := -Og -g3 -fno-omit-frame-pointer
ASAN_FLAGS     := -Og -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
COVERAGE_FLAGS := -Og -g --coverage

.PHONY: all release debug asan win98 test test-junit coverage coverage-run \
        analyze tidy clean help variant

all: release

release:
	@$(MAKE) --no-print-directory variant VARIANT=release \
	    VARIANT_FLAGS="$(RELEASE_FLAGS)" OUT=$(BIN_DIR)/w98link

debug:
	@$(MAKE) --no-print-directory variant VARIANT=debug \
	    VARIANT_FLAGS="$(DEBUG_FLAGS)" OUT=$(BIN_DIR)/w98link-debug

asan:
	@$(MAKE) --no-print-directory variant VARIANT=asan \
	    VARIANT_FLAGS="$(ASAN_FLAGS)" OUT=$(BIN_DIR)/w98link-asan

OBJ_DIR := $(BUILD_DIR)/$(VARIANT)
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(NATIVE_SOURCES))

variant: $(OUT)

$(OUT): $(OBJECTS) $(ZLIB_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS_COMMON) $(VARIANT_FLAGS) -o $@ $(OBJECTS) $(ZLIB_LIB)

ZLIB_OBJECTS := $(patsubst $(ZLIB_DIR)/%.c,$(BUILD_DIR)/zlib/%.o,$(ZLIB_SOURCES))

$(ZLIB_LIB): $(ZLIB_OBJECTS)
	@rm -f $@
	ar rcs $@ $(ZLIB_OBJECTS)

$(BUILD_DIR)/zlib/%.o: $(ZLIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -I$(ZLIB_DIR) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_COMMON) $(CFLAGS_COMMON) $(VARIANT_FLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR):
	@mkdir -p $@

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

# ---------------------------------------------------------------------------
# Win98 agent (Open Watcom cross-build)
# ---------------------------------------------------------------------------

# Watcom lives outside the system package manager, unpacked from the upstream
# release; WATCOM can be overridden to point at another install.
WATCOM   ?= $(HOME)/.local/opt/watcom
WAT_ENV  := WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h:$(WATCOM)/h/nt \
            PATH=$(WATCOM)/binl64:$(WATCOM)/binl:$$PATH
WAT_DIR  := $(BUILD_DIR)/win98
AGENT    := $(BIN_DIR)/W98AGENT.EXE

# -bt=nt/-l=nt is Watcom's Win32 console target, which covers Win9x as well as
# NT. -za99 is its C99 mode; -w4 -we is the closest it has to -Wall -Werror.
# -k sets the main thread's stack explicitly (1 MiB) rather than trusting the
# linker default; a few functions keep a maximum-size record on the stack.
WAT_FLAGS := -q -bt=nt -l=nt -za99 -w4 -we -ox -k1048576 -I$(INC_DIR) \
             '-dW98LINK_VERSION="$(VERSION)_$(BUILD_ID).win98"'

win98: $(AGENT)

WAT_ZLIB_FLAGS := -q -bt=nt -za99 -w0 -ox -I$(ZLIB_DIR)
WAT_ZLIB_OBJS  := $(patsubst $(ZLIB_DIR)/%.c,$(WAT_DIR)/zlib/%.obj,$(ZLIB_SOURCES))

$(WAT_DIR)/zlib/%.obj: $(ZLIB_DIR)/%.c
	@test -x $(WATCOM)/binl64/wcl386 || { \
	    echo "Open Watcom not found at $(WATCOM); set WATCOM=" >&2; exit 1; }
	@mkdir -p $(dir $@)
	$(WAT_ENV) wcl386 -c $(WAT_ZLIB_FLAGS) -fo=$@ $<

$(AGENT): $(CORE_SOURCES) $(AGENT_SOURCES) $(WAT_ZLIB_OBJS) | $(BIN_DIR)
	@mkdir -p $(WAT_DIR)
	$(WAT_ENV) wcl386 $(WAT_FLAGS) -I$(ZLIB_DIR) -fo=$(WAT_DIR)/ -fe=$@ \
	    $(CORE_SOURCES) $(AGENT_SOURCES) $(WAT_ZLIB_OBJS)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------

TEST_SOURCES := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS    := $(patsubst $(TEST_DIR)/test_%.c,$(BIN_DIR)/test_%,$(TEST_SOURCES))

$(BIN_DIR)/test_%: $(TEST_DIR)/test_%.c $(LIB_SOURCES) $(ZLIB_LIB) | $(BIN_DIR)
	$(CC) $(CPPFLAGS_COMMON) -DVARIANT=debug $(CFLAGS_COMMON) $(DEBUG_FLAGS) \
	    -pthread -I$(TEST_DIR) -o $@ $< $(LIB_SOURCES) $(ZLIB_LIB) -lcmocka

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
	    echo "== $$t"; $$t || fail=1; \
	done; exit $$fail

# cmocka's XML mode replaces stdout entirely, so CI gets its own target and
# `make test` stays readable at a terminal.
test-junit: $(TEST_BINS)
	@mkdir -p $(BUILD_DIR)/junit
	@fail=0; for t in $(TEST_BINS); do \
	    name=$$(basename $$t); \
	    CMOCKA_MESSAGE_OUTPUT=xml \
	    CMOCKA_XML_FILE=$(BUILD_DIR)/junit/$$name.junit.xml $$t || fail=1; \
	done; exit $$fail

coverage:
	@$(MAKE) --no-print-directory coverage-run VARIANT=coverage

COV_BINS := $(patsubst $(TEST_DIR)/test_%.c,$(BUILD_DIR)/coverage/test_%,$(TEST_SOURCES))

$(BUILD_DIR)/coverage/test_%: $(TEST_DIR)/test_%.c $(LIB_SOURCES) $(ZLIB_LIB)
	@mkdir -p $(BUILD_DIR)/coverage
	$(CC) $(CPPFLAGS_COMMON) -std=c11 $(COVERAGE_FLAGS) -I$(TEST_DIR) \
	    -pthread -o $@ $< $(LIB_SOURCES) $(ZLIB_LIB) -lcmocka

coverage-run: $(COV_BINS)
	@for t in $(COV_BINS); do $$t >/dev/null || true; done
	@gcovr --root . --filter 'src/core/.*' --filter 'src/posix/.*' \
	    --fail-under-line 80 --print-summary

# ---------------------------------------------------------------------------
# static analysis
# ---------------------------------------------------------------------------

analyze:
	@echo "== cppcheck"
	@cppcheck --enable=warning,performance,portability \
	    --check-level=exhaustive --inline-suppr --error-exitcode=1 --quiet \
	    --suppress=missingIncludeSystem --suppress=checkersReport \
	    -I$(INC_DIR) $(SRC_DIR)/core $(SRC_DIR)/client $(wildcard $(SRC_DIR)/posix)
	@echo "== gcc -fanalyzer"
	@$(CC) $(CPPFLAGS_COMMON) -DVARIANT=analyze $(CFLAGS_COMMON) -O2 \
	    -fanalyzer -fsyntax-only $(NATIVE_SOURCES)
	@echo "== stack usage"
	@rm -rf $(BUILD_DIR)/stack && mkdir -p $(BUILD_DIR)/stack
	@for f in $(NATIVE_SOURCES); do \
	    obj=$(BUILD_DIR)/stack/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    $(CC) $(CPPFLAGS_COMMON) -DVARIANT=stack $(CFLAGS_COMMON) -O2 \
	        -fstack-usage -c $$f -o $$obj || exit 1; \
	done
	@$(PYTHON) tools/check_stack.py $(BUILD_DIR)/stack

tidy:
	@clang-tidy $(NATIVE_SOURCES) -- $(CPPFLAGS_COMMON) -DVARIANT=tidy -std=c11

# ---------------------------------------------------------------------------
# housekeeping
# ---------------------------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

help:
	@sed -n '2,14p' Makefile | sed 's/^# \{0,1\}//'
