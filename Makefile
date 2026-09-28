# devlink - compressed file, command and sync link to an old Windows machine
#
# make              build the Linux client into build/bin/
# make debug asan   the other Linux build variants
# make agent        cross-build the Windows agent with Open Watcom
# make agent-linux  the Linux agent, static; CC= for a cross compiler
# make test         build and run the cmocka suite
# make test-junit   the same suite, with JUnit XML under build/junit/
# make coverage     run the suite under gcov and enforce the line gate
# make analyze      cppcheck, gcc -fanalyzer and the stack-usage check
# make tidy         clang-tidy over the sources, tests and tools
# make clean        remove build/
#
# Build artifacts live under build/, one subdirectory per variant, with the
# ready-to-run binaries (the client, DEVLINK.EXE and devlink-agent) in
# build/bin/.

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
# Two front ends over one agent: a console one and a windowed one. Everything
# but the entry file is shared, so only the last source and the link target
# differ between them.
AGENT_ENTRY    := $(SRC_DIR)/agent/main.c
AGENTW_ENTRY   := $(SRC_DIR)/agent/gui.c
AGENT_SHARED   := $(filter-out $(AGENT_ENTRY) $(AGENTW_ENTRY),\
                  $(wildcard $(SRC_DIR)/agent/*.c)) $(wildcard $(SRC_DIR)/win32/*.c)
AGENT_SOURCES  := $(AGENT_ENTRY) $(AGENT_SHARED)
AGENTW_SOURCES := $(AGENTW_ENTRY) $(AGENT_SHARED)
# The Linux agent: the same core over the POSIX layer, minus what only the
# client uses (the multiplexer and its socket).
LINUX_AGENT_SOURCES := $(CORE_SOURCES) $(SRC_DIR)/linux/main.c $(SRC_DIR)/linux/tui_term.c \
                       $(addprefix $(SRC_DIR)/posix/,clock.c fs.c proc.c random.c serial.c tcp.c \
                       wait.c)

# Vendored zlib: built once with its own flags into an archive, never subject
# to this project's warnings, analysis or coverage (third_party/zlib/VENDORED.md).
ZLIB_DIR     := third_party/zlib
ZLIB_SOURCES := $(addprefix $(ZLIB_DIR)/,adler32.c crc32.c deflate.c inffast.c \
                inflate.c inftrees.c trees.c zutil.c)
ZLIB_LIB     := $(BUILD_DIR)/zlib/libz.a

# Vendored Monocypher, for X25519 and ChaCha20-Poly1305 under the TCP link's
# Noise handshake. Treated exactly as zlib is (third_party/monocypher/VENDORED.md).
MONO_DIR     := third_party/monocypher
MONO_SOURCES := $(MONO_DIR)/monocypher.c
MONO_LIB     := $(BUILD_DIR)/monocypher/libmonocypher.a

# Every native link takes both archives.
VENDOR_LIBS  := $(ZLIB_LIB) $(MONO_LIB)

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

# -isystem, so zlib.h and monocypher.h are held to system-header rules rather
# than ours.
CPPFLAGS_COMMON := -I$(INC_DIR) -isystem $(ZLIB_DIR) -isystem $(MONO_DIR) -D_GNU_SOURCE \
                   -DDEVLINK_VERSION='"$(VERSION)_$(BUILD_ID).$(VARIANT)"'
CFLAGS_COMMON   := -std=c11 $(WARNINGS) $(HARDENING) -Werror

RELEASE_FLAGS  := -O3 -flto -DNDEBUG
DEBUG_FLAGS    := -Og -g3 -fno-omit-frame-pointer
ASAN_FLAGS     := -Og -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
COVERAGE_FLAGS := -Og -g --coverage

.PHONY: all release debug asan agent agent-linux dist test test-junit coverage coverage-run \
        analyze tidy clean help variant

all: release

release:
	@$(MAKE) --no-print-directory variant VARIANT=release \
	    VARIANT_FLAGS="$(RELEASE_FLAGS)" OUT=$(BIN_DIR)/devlink

debug:
	@$(MAKE) --no-print-directory variant VARIANT=debug \
	    VARIANT_FLAGS="$(DEBUG_FLAGS)" OUT=$(BIN_DIR)/devlink-debug

asan:
	@$(MAKE) --no-print-directory variant VARIANT=asan \
	    VARIANT_FLAGS="$(ASAN_FLAGS)" OUT=$(BIN_DIR)/devlink-asan

OBJ_DIR := $(BUILD_DIR)/$(VARIANT)
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(NATIVE_SOURCES))

variant: $(OUT)

$(OUT): $(OBJECTS) $(VENDOR_LIBS) | $(BIN_DIR)
	$(CC) $(CFLAGS_COMMON) $(VARIANT_FLAGS) -o $@ $(OBJECTS) $(VENDOR_LIBS)

ZLIB_OBJECTS := $(patsubst $(ZLIB_DIR)/%.c,$(BUILD_DIR)/zlib/%.o,$(ZLIB_SOURCES))

$(ZLIB_LIB): $(ZLIB_OBJECTS)
	@rm -f $@
	ar rcs $@ $(ZLIB_OBJECTS)

$(BUILD_DIR)/zlib/%.o: $(ZLIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -I$(ZLIB_DIR) -c $< -o $@

MONO_OBJECTS := $(patsubst $(MONO_DIR)/%.c,$(BUILD_DIR)/monocypher/%.o,$(MONO_SOURCES))

$(MONO_LIB): $(MONO_OBJECTS)
	@rm -f $@
	ar rcs $@ $(MONO_OBJECTS)

$(BUILD_DIR)/monocypher/%.o: $(MONO_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -I$(MONO_DIR) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_COMMON) $(CFLAGS_COMMON) $(VARIANT_FLAGS) -MMD -MP -c $< -o $@

# BUILD_ID is stamped into version.c alone, so make has no reason to
# recompile it when any other file changes, and a freshly built client then
# reports a build id older than itself. That is not cosmetic: the build id is
# how a deployment to the target is confirmed. Anything that goes into the
# binary is a prerequisite here, so the stamp is refreshed exactly when a
# relink was going to happen anyway. (The Win98 agent needs none of this: one
# wcl386 call compiles every file every time.)
$(OBJ_DIR)/core/version.o: $(filter-out $(SRC_DIR)/core/version.c,$(NATIVE_SOURCES)) \
                           $(wildcard $(INC_DIR)/devlink/*.h) release_version

$(BIN_DIR):
	@mkdir -p $@

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

# ---------------------------------------------------------------------------
# Windows agent (Open Watcom cross-build)
# ---------------------------------------------------------------------------

# Watcom lives outside the system package manager, unpacked from the upstream
# release; WATCOM can be overridden to point at another install.
WATCOM   ?= $(HOME)/.local/opt/watcom
WAT_ENV  := WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h:$(WATCOM)/h/nt \
            PATH=$(WATCOM)/binl64:$(WATCOM)/binl:$$PATH
WAT_DIR  := $(BUILD_DIR)/agent
AGENT    := $(BIN_DIR)/DEVLINK.EXE
AGENTW   := $(BIN_DIR)/DEVLINKW.EXE

# -bt=nt is Watcom's Win32 target, which covers Win9x as well as NT; the
# subsystem comes from -l=nt or -l=nt_win in the link rules below. -za99 is its
# C99 mode; -w4 -we is the closest it has to -Wall -Werror. -k sets the main
# thread's stack explicitly (1 MiB) rather than trusting the linker default; a
# few functions keep a maximum-size record on the stack.
WAT_FLAGS := -q -bt=nt -za99 -w4 -we -ox -k1048576 -I$(INC_DIR) \
             '-dDEVLINK_VERSION="$(VERSION)_$(BUILD_ID).agent"'

agent: $(AGENT) $(AGENTW)

# Resources, shared by both binaries: the icon and a version block. The icon is
# drawn at build time rather than committed, so nothing in the repository is a
# binary nobody can review. VERSIONINFO wants four numbers, which is the semver
# with a build field of zero after it.
comma    := ,
WAT_ICON := $(WAT_DIR)/devlink.ico
WAT_RES  := $(WAT_DIR)/devlink.res
VER_COMMA := $(subst .,$(comma),$(VERSION)),0

$(WAT_ICON): tools/make_icon.py
	@mkdir -p $(dir $@)
	@$(PYTHON) tools/make_icon.py $@

$(WAT_RES): $(SRC_DIR)/agent/devlink.rc $(WAT_ICON) release_version
	@mkdir -p $(dir $@)
	$(WAT_ENV) wrc -q -r -bt=nt -i=$(WAT_DIR) -dDEVLINK_VER_COMMA="$(VER_COMMA)" \
	    '-dDEVLINK_VER_TEXT="$(VERSION)_$(BUILD_ID)"' -fo=$@ $<

WAT_ZLIB_FLAGS := -q -bt=nt -za99 -w0 -ox -I$(ZLIB_DIR)
WAT_ZLIB_OBJS  := $(patsubst $(ZLIB_DIR)/%.c,$(WAT_DIR)/zlib/%.obj,$(ZLIB_SOURCES))
WAT_MONO_FLAGS := -q -bt=nt -za99 -w0 -ox -I$(MONO_DIR)
WAT_MONO_OBJS  := $(patsubst $(MONO_DIR)/%.c,$(WAT_DIR)/monocypher/%.obj,$(MONO_SOURCES))
WAT_VENDOR_OBJS := $(WAT_ZLIB_OBJS) $(WAT_MONO_OBJS)

$(WAT_DIR)/zlib/%.obj: $(ZLIB_DIR)/%.c
	@test -x $(WATCOM)/binl64/wcl386 || { \
	    echo "Open Watcom not found at $(WATCOM); set WATCOM=" >&2; exit 1; }
	@mkdir -p $(dir $@)
	$(WAT_ENV) wcl386 -c $(WAT_ZLIB_FLAGS) -fo=$@ $<

$(WAT_DIR)/monocypher/%.obj: $(MONO_DIR)/%.c
	@test -x $(WATCOM)/binl64/wcl386 || { \
	    echo "Open Watcom not found at $(WATCOM); set WATCOM=" >&2; exit 1; }
	@mkdir -p $(dir $@)
	$(WAT_ENV) wcl386 -c $(WAT_MONO_FLAGS) -fo=$@ $<

$(AGENT): $(CORE_SOURCES) $(AGENT_SOURCES) $(WAT_VENDOR_OBJS) $(WAT_RES) | $(BIN_DIR)
	@mkdir -p $(WAT_DIR)
	$(WAT_ENV) wcl386 $(WAT_FLAGS) -l=nt -I$(ZLIB_DIR) -I$(MONO_DIR) -fo=$(WAT_DIR)/ -fe=$@ \
	    $(CORE_SOURCES) $(AGENT_SOURCES) $(WAT_VENDOR_OBJS)
	$(WAT_ENV) wrc -q -bt=nt $(WAT_RES) $@
	@chmod 755 $@

# -l=nt_win is the same Win32 target with the windowed subsystem bit, and its
# default library list already carries comctl32 and shell32. Its objects go in
# their own directory: the same core file compiled for either front end has the
# same object name, and sharing the directory would mean each build overwriting
# the other's and neither being rebuilt when only the entry file changed.
$(AGENTW): $(CORE_SOURCES) $(AGENTW_SOURCES) $(WAT_VENDOR_OBJS) $(WAT_RES) | $(BIN_DIR)
	@mkdir -p $(WAT_DIR)/win
	$(WAT_ENV) wcl386 $(WAT_FLAGS) -l=nt_win -I$(ZLIB_DIR) -I$(MONO_DIR) -fo=$(WAT_DIR)/win/ -fe=$@ \
	    $(CORE_SOURCES) $(AGENTW_SOURCES) $(WAT_VENDOR_OBJS)
	$(WAT_ENV) wrc -q -bt=nt $(WAT_RES) $@
	@chmod 755 $@

# ---------------------------------------------------------------------------
# Linux agent
# ---------------------------------------------------------------------------

# For devlink-rescue, a small Linux image that boots on a machine whose own OS
# will not. Static, so it needs nothing from the image but a kernel. CC is
# whatever the image's toolchain is (an i586 musl gcc from Buildroot), and
# LINUX_AGENT_ARCH carries its CPU flags; with neither set it builds for this
# machine, which is how it is checked here. Its objects, zlib's included, live
# apart from everything else because they may be for another architecture.
# DEVLINK_AGENT is what lets this build, and no other POSIX one, set the clock.
LINUX_AGENT      := $(BIN_DIR)/devlink-agent
LINUX_AGENT_DIR  := $(BUILD_DIR)/linux-agent
LINUX_AGENT_ARCH ?=
# -pthread: serve --tui runs the agent loop on a thread beside the screen.
LINUX_AGENT_CFLAGS := $(LINUX_AGENT_ARCH) -DDEVLINK_AGENT -pthread
LINUX_AGENT_CPPFLAGS := -I$(INC_DIR) -isystem $(ZLIB_DIR) -isystem $(MONO_DIR) -D_GNU_SOURCE \
                        -DDEVLINK_VERSION='"$(VERSION)_$(BUILD_ID).linux"'
LINUX_AGENT_OBJS := $(patsubst $(SRC_DIR)/%.c,$(LINUX_AGENT_DIR)/%.o,$(LINUX_AGENT_SOURCES)) \
                    $(patsubst $(ZLIB_DIR)/%.c,$(LINUX_AGENT_DIR)/zlib/%.o,$(ZLIB_SOURCES)) \
                    $(patsubst $(MONO_DIR)/%.c,$(LINUX_AGENT_DIR)/monocypher/%.o,$(MONO_SOURCES))

agent-linux: $(LINUX_AGENT)

$(LINUX_AGENT): $(LINUX_AGENT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS_COMMON) $(RELEASE_FLAGS) $(LINUX_AGENT_CFLAGS) -static -o $@ \
	    $(LINUX_AGENT_OBJS)

$(LINUX_AGENT_DIR)/zlib/%.o: $(ZLIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w $(LINUX_AGENT_ARCH) -I$(ZLIB_DIR) -c $< -o $@

$(LINUX_AGENT_DIR)/monocypher/%.o: $(MONO_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w $(LINUX_AGENT_ARCH) -I$(MONO_DIR) -c $< -o $@

$(LINUX_AGENT_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LINUX_AGENT_CPPFLAGS) $(CFLAGS_COMMON) $(RELEASE_FLAGS) \
	    $(LINUX_AGENT_CFLAGS) -MMD -MP -c $< -o $@

# The same stamp rule as the client's version.o, for the same reason.
$(LINUX_AGENT_DIR)/core/version.o: $(filter-out $(SRC_DIR)/core/version.c,$(LINUX_AGENT_SOURCES)) \
                                   $(wildcard $(INC_DIR)/devlink/*.h) release_version

# A look at the windowed agent without a Windows machine: Wine draws it on a
# headless X server and the framebuffer becomes a PNG. Nothing in DEVLINKW.EXE
# is reachable from the test suite, so this is how it is checked at all. Wine
# and Xvfb are not build dependencies and this is in no gate; it skips itself
# where they are missing.
.PHONY: gui-preview
gui-preview: $(AGENTW)
	@tools/gui-preview.sh $(BUILD_DIR)/preview/agent.png

# The Win32 TCP transport, run under Wine: against itself, and against the
# POSIX one in both directions. One test program, tools/wintcp_check.c, built
# twice: by Watcom over src/win32/tcp.c and by gcc over src/posix/tcp.c. Like
# gui-preview it is evidence rather than a gate, and skips itself without Wine.
WINTCP_DIR    := $(BUILD_DIR)/wintcp
WINTCP_EXE    := $(WINTCP_DIR)/WINTCP.EXE
WINTCP_NATIVE := $(WINTCP_DIR)/wintcp
WINTCP_CORE   := $(addprefix $(SRC_DIR)/core/,status.c tcp_spec.c)

.PHONY: wintcp-check
wintcp-check: $(WINTCP_EXE) $(WINTCP_NATIVE)
	@tools/wintcp-check.sh $(WINTCP_EXE) $(WINTCP_NATIVE)

$(WINTCP_EXE): tools/wintcp_check.c $(WINTCP_CORE) $(SRC_DIR)/win32/tcp.c $(SRC_DIR)/win32/clock.c
	@mkdir -p $(WINTCP_DIR)/obj
	$(WAT_ENV) wcl386 $(WAT_FLAGS) -l=nt -fo=$(WINTCP_DIR)/obj/ -fe=$@ $^

$(WINTCP_NATIVE): tools/wintcp_check.c $(WINTCP_CORE) $(SRC_DIR)/posix/tcp.c $(SRC_DIR)/posix/clock.c
	@mkdir -p $(WINTCP_DIR)
	$(CC) $(CPPFLAGS_COMMON) -DVARIANT=wintcp $(CFLAGS_COMMON) -O2 -o $@ $^

# The measurements behind docs/plan.md step 9g: key derivation and deflate
# speed on the machine it runs on. A tool, in no gate; run TCPBENCH.EXE on the
# target (put it there with devlink put) and tcpbench here to compare.
.PHONY: tcpbench
BENCH_DIR  := $(BUILD_DIR)/tcpbench
BENCH_CORE := $(addprefix $(SRC_DIR)/core/,kdf.c noise.c sha256.c status.c)
tcpbench: $(BENCH_DIR)/TCPBENCH.EXE $(BENCH_DIR)/tcpbench

$(BENCH_DIR)/TCPBENCH.EXE: tools/tcpbench.c $(BENCH_CORE) $(SRC_DIR)/win32/clock.c \
                           $(SRC_DIR)/win32/random.c $(WAT_VENDOR_OBJS)
	@mkdir -p $(BENCH_DIR)/obj
	$(WAT_ENV) wcl386 $(WAT_FLAGS) -l=nt -I$(ZLIB_DIR) -I$(MONO_DIR) -fo=$(BENCH_DIR)/obj/ \
	    -fe=$@ $^

$(BENCH_DIR)/tcpbench: tools/tcpbench.c $(BENCH_CORE) $(SRC_DIR)/posix/clock.c \
                       $(SRC_DIR)/posix/random.c $(VENDOR_LIBS)
	@mkdir -p $(BENCH_DIR)
	$(CC) $(CPPFLAGS_COMMON) $(CFLAGS_COMMON) -O2 -o $@ \
	    $(filter %.c,$^) $(VENDOR_LIBS)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------

TEST_SOURCES := $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS    := $(patsubst $(TEST_DIR)/test_%.c,$(BIN_DIR)/test_%,$(TEST_SOURCES))

# The library is compiled once and every test links against it. It used to be
# compiled inside each test binary's own command, which meant 22 sources times
# 15 tests, 330 compilations, every time a header changed, and again with
# instrumentation for coverage. The .d files these objects produce also let
# make rebuild only what a header actually touched.
TEST_DEPS := $(wildcard $(TEST_DIR)/*.h)
TEST_OBJ_DIR := $(BUILD_DIR)/testlib
TEST_LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(TEST_OBJ_DIR)/%.o,$(LIB_SOURCES))

$(TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_COMMON) -DVARIANT=debug $(CFLAGS_COMMON) $(DEBUG_FLAGS) \
	    -pthread -MMD -MP -c $< -o $@

$(BIN_DIR)/test_%: $(TEST_DIR)/test_%.c $(TEST_LIB_OBJS) $(TEST_DEPS) $(VENDOR_LIBS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS_COMMON) -DVARIANT=debug $(CFLAGS_COMMON) $(DEBUG_FLAGS) \
	    -pthread -I$(TEST_DIR) -o $@ $< $(TEST_LIB_OBJS) $(VENDOR_LIBS) -lcmocka

# test_exec_tcp is test_exec.c compiled a second way, by #include.
$(BIN_DIR)/test_exec_tcp: $(TEST_DIR)/test_exec.c

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

# Everything the target needs, in the layout it is copied in: the agent, its
# launcher and the instructions that go with it. DEST can point straight at a
# flash drive (make dist DEST=/run/media/me/STICK).
DEST ?= $(BUILD_DIR)/dist

dist: $(AGENT) $(AGENTW)
	@mkdir -p $(DEST)/DEVLINK
	cp $(AGENT) $(AGENTW) $(DEST)/DEVLINK/
	cp dist/AGENT.BAT dist/README.TXT $(DEST)/DEVLINK/
	@echo "staged in $(DEST)/DEVLINK: copy that folder to C:\\ on the target"

coverage:
	@$(MAKE) --no-print-directory coverage-run VARIANT=coverage

COV_BINS := $(patsubst $(TEST_DIR)/test_%.c,$(BUILD_DIR)/coverage/test_%,$(TEST_SOURCES))

COV_OBJ_DIR := $(BUILD_DIR)/covlib
COV_LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(COV_OBJ_DIR)/%.o,$(LIB_SOURCES))

$(COV_OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS_COMMON) -std=c11 $(COVERAGE_FLAGS) -pthread -MMD -MP -c $< -o $@

$(BUILD_DIR)/coverage/test_%: $(TEST_DIR)/test_%.c $(COV_LIB_OBJS) $(TEST_DEPS) $(VENDOR_LIBS)
	@mkdir -p $(BUILD_DIR)/coverage
	$(CC) $(CPPFLAGS_COMMON) -std=c11 $(COVERAGE_FLAGS) -I$(TEST_DIR) \
	    -pthread -o $@ $< $(COV_LIB_OBJS) $(VENDOR_LIBS) -lcmocka

coverage-run: $(COV_BINS)
	@# Counters from an earlier run, or from an earlier layout of these
	@# objects, make gcov report a stamp mismatch and gcovr fail with an
	@# error that names neither. Each run starts from nothing.
	@find $(COV_OBJ_DIR) $(BUILD_DIR)/coverage -name '*.gcda' -delete 2>/dev/null || true
	@for t in $(COV_BINS); do $$t >/dev/null || true; done
# gcov's counters wrap on the hottest inlined functions (sha256's rotations,
# once a test hashes 100 MB), and gcovr treats the wrapped value as a parse
# error. Warning once per file keeps the run honest without failing the gate
# over a known gcov bug (gcc PR 68080).
	@gcovr --root . --filter 'src/core/.*' --filter 'src/posix/.*' \
	    --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file \
	    --fail-under-line 80 --print-summary

# ---------------------------------------------------------------------------
# static analysis
# ---------------------------------------------------------------------------

analyze:
	@echo "== cppcheck"
	@cppcheck --enable=warning,performance,portability \
	    --check-level=exhaustive --inline-suppr --error-exitcode=1 --quiet \
	    --suppress=missingIncludeSystem --suppress=checkersReport \
	    -I$(INC_DIR) $(SRC_DIR)/core $(SRC_DIR)/client $(SRC_DIR)/posix $(SRC_DIR)/linux
	@echo "== gcc -fanalyzer"
	@$(CC) $(CPPFLAGS_COMMON) -DVARIANT=analyze $(CFLAGS_COMMON) -O2 \
	    -fanalyzer -fsyntax-only $(NATIVE_SOURCES)
	@echo "== gcc -fanalyzer (Linux agent)"
	@$(CC) $(CPPFLAGS_COMMON) -DVARIANT=analyze -DDEVLINK_AGENT $(CFLAGS_COMMON) -O2 \
	    -fanalyzer -fsyntax-only $(SRC_DIR)/linux/main.c $(SRC_DIR)/linux/tui_term.c $(SRC_DIR)/posix/clock.c
	@echo "== stack usage"
	@rm -rf $(BUILD_DIR)/stack && mkdir -p $(BUILD_DIR)/stack
	@for f in $(NATIVE_SOURCES) $(SRC_DIR)/linux/main.c $(SRC_DIR)/linux/tui_term.c; do \
	    obj=$(BUILD_DIR)/stack/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    $(CC) $(CPPFLAGS_COMMON) -DVARIANT=stack $(CFLAGS_COMMON) -O2 \
	        -fstack-usage -c $$f -o $$obj || exit 1; \
	done
	@$(PYTHON) tools/check_stack.py $(BUILD_DIR)/stack

# The Win32 half is linted too, against mingw-w64's headers: clang cannot
# parse windows.h otherwise, which is how half the shipped agent went
# unchecked until a stray identical-branch if in wl_fs_list turned up in an
# editor. It is read with mingw's declarations and built with Watcom's, so a
# finding here is about our code, not about which header set is in use; the
# one flag this costs is documented in .clangd. Skipped with a notice when no
# mingw headers are installed, since it must not stop the gate on a machine
# that has none.
WIN_TIDY_DIRS := /usr/i686-w64-mingw32/include /usr/i686-w64-mingw32/usr/include \
                 /usr/local/i686-w64-mingw32/include \
                 /usr/lib/mingw64-toolchain/x86_64-w64-mingw32/include
WIN_TIDY_INC  := $(foreach d,$(WIN_TIDY_DIRS),$(if $(wildcard $(d)),-isystem $(d)))

# One clang-tidy per core: it analyses files serially within one invocation,
# and the analyser is the slowest gate by a wide margin.
JOBS := $(shell nproc 2>/dev/null || echo 4)

tidy:
	@echo $(NATIVE_SOURCES) | tr ' ' '\n' | xargs -P$(JOBS) -I{} \
	    clang-tidy {} -- $(CPPFLAGS_COMMON) -DVARIANT=tidy -std=c11
	@echo "== Linux agent"
	@echo $(SRC_DIR)/linux/main.c $(SRC_DIR)/linux/tui_term.c $(SRC_DIR)/posix/clock.c | tr ' ' '\n' | xargs -P$(JOBS) -I{} \
	    clang-tidy {} -- $(CPPFLAGS_COMMON) -DVARIANT=tidy -DDEVLINK_AGENT -std=c11
	@echo "== tests"
	@echo $(TEST_SOURCES) | tr ' ' '\n' | xargs -P$(JOBS) -I{} \
	    clang-tidy {} -- $(CPPFLAGS_COMMON) -DVARIANT=tidy -I$(TEST_DIR) \
	    -include $(TEST_DIR)/tidy_asserts.h -std=c11
	@echo "== tools"
	@echo tools/tcpbench.c tools/wintcp_check.c | tr ' ' '\n' | xargs -P$(JOBS) -I{} \
	    clang-tidy {} -- $(CPPFLAGS_COMMON) -DVARIANT=tidy -std=c11
	@if [ -z "$(WIN_TIDY_INC)" ]; then \
	    echo "== win32: skipped, no mingw-w64 headers found (install them to lint it)"; \
	else \
	    echo "== win32"; \
	    echo $(sort $(AGENT_SOURCES) $(AGENTW_SOURCES)) | tr ' ' '\n' | xargs -P$(JOBS) -I{} \
	        clang-tidy {} -- --target=i686-w64-mingw32 $(WIN_TIDY_INC) \
	        -I$(INC_DIR) -isystem $(ZLIB_DIR) -isystem $(MONO_DIR) -D_WIN32_WINNT=0x0500 -DVARIANT=tidy \
	        -DDEVLINK_VERSION='"0.0.0_tidy"' -std=c11 -Wno-cast-qual; \
	fi

# ---------------------------------------------------------------------------
# housekeeping
# ---------------------------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

help:
	@sed -n '2,16p' Makefile | sed 's/^# \{0,1\}//'
