CC  ?= cc
CXX ?= c++
AR  ?= ar

PREFIX ?= /usr/local

# ---------------------------------------------------------------------------
# libjami prefix
#
# libjami is consumed as a pre-built static prefix at $(JAMI_PREFIX), which
# defaults to ${XDG_CACHE_HOME:-$HOME/.cache}/resonator/libjami/<key>/ where
# <key> selects host vs iOS slice:
#
#   host:                 <sha>/
#   ios-device:           <sha>-ios-device-arm64/
#   ios-simulator:        <sha>-ios-sim-fat/
#
# <sha> is the line read from JAMI_VERSION. The prefix is laid out as:
#
#   $(JAMI_PREFIX)/lib/libjami-core.a   — the core library
#   $(JAMI_PREFIX)/lib/lib<contrib>.a   — contrib deps (~39 archives)
#   $(JAMI_PREFIX)/include/jami/*.h     — public headers
#
# Populate the prefix one of three ways:
#   make libjami                          — fetch pre-built tarball; fall back
#                                           to source build (recommended; ~30s
#                                           when an artifact is published)
#   make libjami-fetch                    — download a pre-built tarball only
#                                           (no fallback)
#   make libjami-build                    — clone jami-daemon at the pinned SHA
#                                           outside the repo, build
#                                           hermetically, stage into the cache
#                                           (cold build: 1-3 hours)
#
# Override the platform via the PLATFORM variable (default `host`):
#   make libjami PLATFORM=ios-device      — iPhone arm64 slice
#   make libjami PLATFORM=ios-simulator   — Simulator arm64+x86_64 (fat)
#
# Pre-built tarballs are produced by .github/workflows/build-libjami-artifacts.yml
# whenever JAMI_VERSION changes on main; see arch/jami-migration.md D21 for the
# overall design rationale.
# ---------------------------------------------------------------------------

PLATFORM ?= host

# JAMI_SUFFIX maps the platform onto the cache-key suffix shared with
# tools/{build,fetch}-libjami.sh. Host: empty (prefix = <sha>). iOS: a
# platform-specific suffix appended to <sha>. An unknown PLATFORM hits the
# `$(error ...)` at parse time.
#
# ios-sim-arm64 / ios-sim-x86_64 are internal single-arch values used by the
# `libcarrier-ios PLATFORM=ios-simulator` recursive build. They share the
# fat libjami prefix (libjami's sim slice is published lipo'd; our carrier
# slice is lipo'd at the end too). End users specify ios-simulator.
#
# `ifeq` blocks (rather than nested `$(if ...)`) avoid the whitespace-leak
# trap where continuation-line indentation gets baked into the suffix value.
ifeq ($(PLATFORM),host)
JAMI_SUFFIX :=
else ifeq ($(PLATFORM),ios-device)
JAMI_SUFFIX := -ios-device-arm64
else ifeq ($(PLATFORM),ios-simulator)
JAMI_SUFFIX := -ios-sim-fat
else ifeq ($(PLATFORM),ios-sim-arm64)
JAMI_SUFFIX := -ios-sim-fat
else ifeq ($(PLATFORM),ios-sim-x86_64)
JAMI_SUFFIX := -ios-sim-fat
else ifeq ($(PLATFORM),android-arm64)
JAMI_SUFFIX := -android-arm64
else
$(error unknown PLATFORM=$(PLATFORM) (host|ios-device|ios-simulator|android-arm64))
endif

JAMI_SHA          := $(shell tr -d '[:space:]' < JAMI_VERSION 2>/dev/null)
XDG_CACHE_HOME    ?= $(HOME)/.cache
JAMI_PREFIX       ?= $(XDG_CACHE_HOME)/resonator/libjami/$(JAMI_SHA)$(JAMI_SUFFIX)
JAMI_INC          = $(JAMI_PREFIX)/include
JAMI_LIB          = $(JAMI_PREFIX)/lib/libjami-core.a
JAMI_CONTRIB_LIB  = $(JAMI_PREFIX)/lib

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# pjsip names its libraries with the GNU arch convention (aarch64) while the
# host triple uses the Apple arch name (arm64). The lib filenames also drop
# the macOS kernel version — see tools/build-libjami.sh's staging step,
# which rewrites kernel-versioned names into a stable suffix so the same
# prefix serves every macOS host.
ifeq ($(UNAME_S), Darwin)
JAMI_PJ_TRIPLE   = $(subst arm64,aarch64,$(UNAME_M))-apple-darwin
else ifeq ($(UNAME_S), Linux)
JAMI_PJ_TRIPLE   = $(UNAME_M)-linux-gnu
endif

# --- Serd (compiled from submodule sources; still C) ---
SERD_DIR     = third_party/serd
SERD_INC     = $(SERD_DIR)/include
SERD_SRC_DIR = $(SERD_DIR)/src

SRC_DIR   = src
INC_DIR   = include

# BUILD_DIR is platform-suffixed so iOS objects don't clobber host ones —
# `make all` (host) keeps the historical `build/` path, and each iOS slice
# gets its own dir. The ios-simulator fat dir is produced by lipo'ing the
# two single-arch dirs (see the libcarrier-ios target below).
ifeq ($(PLATFORM),host)
BUILD_DIR := build
else ifeq ($(PLATFORM),ios-device)
BUILD_DIR := build-ios-device-arm64
else ifeq ($(PLATFORM),ios-simulator)
BUILD_DIR := build-ios-sim-fat
else ifeq ($(PLATFORM),ios-sim-arm64)
BUILD_DIR := build-ios-sim-arm64
else ifeq ($(PLATFORM),ios-sim-x86_64)
BUILD_DIR := build-ios-sim-x86_64
else ifeq ($(PLATFORM),android-arm64)
BUILD_DIR := build-android-arm64
else
$(error unknown PLATFORM=$(PLATFORM) (host|ios-device|ios-simulator|android-arm64))
endif

# ---------------------------------------------------------------------------
# Compile flags
# ---------------------------------------------------------------------------

COMMON_WARN = -Wall -Wextra -Wno-unused-parameter -Wno-nullability-extension
COMMON_INC  = -I$(INC_DIR) -I$(SRC_DIR) -I$(JAMI_INC) -I$(SERD_INC)

# iOS cross-compile: override CC/CXX/AR to xcrun-driven clang and inject
# the right arch + sysroot + version-min flags. Bitcode is on for device
# (Jami's own contribs build with -fembed-bitcode), off for simulator.
# The 'ios-simulator' composite platform doesn't compile directly — it
# drives recursive ios-sim-arm64 / ios-sim-x86_64 builds and lipos the
# resulting archives (see libcarrier-ios target).
ifneq ($(filter ios-device ios-sim-arm64 ios-sim-x86_64,$(PLATFORM)),)
ifeq ($(PLATFORM),ios-device)
IOS_SDK   := iphoneos
IOS_ARCH  := arm64
IOS_MIN   := -miphoneos-version-min=14.5
IOS_BC    := -fembed-bitcode
else ifeq ($(PLATFORM),ios-sim-arm64)
IOS_SDK   := iphonesimulator
IOS_ARCH  := arm64
IOS_MIN   := -mios-simulator-version-min=14.5
IOS_BC    :=
else
IOS_SDK   := iphonesimulator
IOS_ARCH  := x86_64
IOS_MIN   := -mios-simulator-version-min=14.5
IOS_BC    :=
endif
IOS_SDKROOT := $(shell xcrun --sdk $(IOS_SDK) --show-sdk-path)
IOS_FLAGS   := $(IOS_MIN) $(IOS_BC) -arch $(IOS_ARCH) -isysroot $(IOS_SDKROOT)

CC  := xcrun -sdk $(IOS_SDK) clang
CXX := xcrun -sdk $(IOS_SDK) clang++
AR  := xcrun -sdk $(IOS_SDK) ar
endif

# Android cross-compile: override CC/CXX/AR to the NDK clang toolchain. The
# versioned clang wrapper (aarch64-linux-android24-clang) bakes in --target,
# the API level, and the sysroot, so no extra arch/isysroot flags are needed —
# only -fPIC, because these objects get archived into libcarrier.a and then
# linked into antenna_ffi.so (a cdylib). Library-only, same as the iOS slice
# (see libcarrier-android); the final libjami link happens in antenna/build.rs.
ifeq ($(PLATFORM),android-arm64)
ANDROID_API_LEVEL ?= 26
ANDROID_HOST_TRIPLE := aarch64-linux-android
ifeq ($(ANDROID_NDK_HOME),)
ANDROID_NDK_HOME := $(ANDROID_NDK)
endif
ifeq ($(ANDROID_NDK_HOME),)
ANDROID_SDK_DIR := $(firstword $(wildcard $(ANDROID_HOME)) $(wildcard $(ANDROID_SDK_ROOT)) $(HOME)/Library/Android/sdk)
ANDROID_NDK_HOME := $(lastword $(sort $(wildcard $(ANDROID_SDK_DIR)/ndk/*)))
endif
ifeq ($(ANDROID_NDK_HOME),)
$(error android build needs an NDK: set ANDROID_NDK_HOME)
endif
ifeq ($(UNAME_S),Darwin)
ANDROID_NDK_HOST_TAG := darwin-x86_64
else
ANDROID_NDK_HOST_TAG := linux-x86_64
endif
ANDROID_NDK_BIN := $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/$(ANDROID_NDK_HOST_TAG)/bin
ANDROID_FLAGS   := -fPIC
CC  := $(ANDROID_NDK_BIN)/$(ANDROID_HOST_TRIPLE)$(ANDROID_API_LEVEL)-clang
CXX := $(ANDROID_NDK_BIN)/$(ANDROID_HOST_TRIPLE)$(ANDROID_API_LEVEL)-clang++
AR  := $(ANDROID_NDK_BIN)/llvm-ar
endif

CFLAGS   ?= -std=c11 -D_DEFAULT_SOURCE $(COMMON_WARN)
CFLAGS   += $(COMMON_INC) -DSERD_STATIC

CXXFLAGS ?= -std=c++20 $(COMMON_WARN)
CXXFLAGS += $(COMMON_INC)

# Append iOS arch/sdk/min-version flags to every compile command. Done as a
# separate `+=` after the host defaults so users can still override CFLAGS
# from the environment for host builds without losing the iOS injection.
ifneq ($(filter ios-device ios-sim-arm64 ios-sim-x86_64,$(PLATFORM)),)
CFLAGS   += $(IOS_FLAGS)
CXXFLAGS += $(IOS_FLAGS)
endif
ifeq ($(PLATFORM),android-arm64)
CFLAGS   += $(ANDROID_FLAGS)
CXXFLAGS += $(ANDROID_FLAGS)
endif

# ---------------------------------------------------------------------------
# Link flags
#
# libjami's transitive dep set is large; we link each contrib lib explicitly
# because libjami does not yet export a cmake target we can consume.
# Missing libs fail at link time with a clear message.
# ---------------------------------------------------------------------------

LDFLAGS  ?=
LDFLAGS  += $(JAMI_LIB)

JAMI_CONTRIB_LIBS = \
    $(JAMI_CONTRIB_LIB)/libdhtnet.a \
    $(JAMI_CONTRIB_LIB)/libopendht.a \
    $(JAMI_CONTRIB_LIB)/libpjsua2-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjsua-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjsip-ua-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjsip-simple-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjsip-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjmedia-codec-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjmedia-audiodev-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjmedia-videodev-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjmedia-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjnath-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpjlib-util-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libpj-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libsrtp-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libyuv-$(JAMI_PJ_TRIPLE).a \
    $(JAMI_CONTRIB_LIB)/libavformat.a \
    $(JAMI_CONTRIB_LIB)/libavcodec.a \
    $(JAMI_CONTRIB_LIB)/libavfilter.a \
    $(JAMI_CONTRIB_LIB)/libavdevice.a \
    $(JAMI_CONTRIB_LIB)/libswresample.a \
    $(JAMI_CONTRIB_LIB)/libswscale.a \
    $(JAMI_CONTRIB_LIB)/libavutil.a \
    $(JAMI_CONTRIB_LIB)/libx264.a \
    $(JAMI_CONTRIB_LIB)/libfmt.a \
    $(JAMI_CONTRIB_LIB)/libhttp_parser.a \
    $(JAMI_CONTRIB_LIB)/libllhttp.a \
    $(JAMI_CONTRIB_LIB)/libnatpmp.a \
    $(JAMI_CONTRIB_LIB)/libsimdutf.a \
    $(JAMI_CONTRIB_LIB)/libixml.a \
    $(JAMI_CONTRIB_LIB)/libupnp.a \
    $(JAMI_CONTRIB_LIB)/libspeex.a \
    $(JAMI_CONTRIB_LIB)/libspeexdsp.a \
    $(JAMI_CONTRIB_LIB)/libminizip.a \
    $(JAMI_CONTRIB_LIB)/libzstd.a \
    $(JAMI_CONTRIB_LIB)/libbzip2.a \
    $(JAMI_CONTRIB_LIB)/libsecp256k1.a \
    $(JAMI_CONTRIB_LIB)/libyaml-cpp.a \
    $(JAMI_CONTRIB_LIB)/libgit2.a \
    $(JAMI_CONTRIB_LIB)/libjsoncpp.a \
    $(JAMI_CONTRIB_LIB)/libopus.a \
    $(JAMI_CONTRIB_LIB)/libvpx.a \
    $(JAMI_CONTRIB_LIB)/libargon2.a \
    $(JAMI_CONTRIB_LIB)/libgnutls.a \
    $(JAMI_CONTRIB_LIB)/libhogweed.a \
    $(JAMI_CONTRIB_LIB)/libnettle.a \
    $(JAMI_CONTRIB_LIB)/libgmp.a \
    $(JAMI_CONTRIB_LIB)/libssl.a \
    $(JAMI_CONTRIB_LIB)/libcrypto.a \
    $(JAMI_CONTRIB_LIB)/libtls.a \
    $(JAMI_CONTRIB_LIB)/libz.a

# libiconv + libcharset are produced by contrib only when --ignore-system-libs
# is passed to bootstrap (which wipes iconv's unconditional PKGS_FOUND on
# POSIX; see tools/build-libjami.sh). When present, libavformat references
# GNU's `_libiconv_*` symbols and resolves them here. When absent, libavformat
# uses system `_iconv_*` resolved via Darwin's -liconv / glibc.
JAMI_CONTRIB_LIBS += $(wildcard $(JAMI_CONTRIB_LIB)/libiconv.a $(JAMI_CONTRIB_LIB)/libcharset.a)

LDFLAGS += $(JAMI_CONTRIB_LIBS)

# All third-party C/C++ deps come from contrib (hermetic build, see D21).
# Only system frameworks + the C runtime are pulled from outside the prefix.
ifeq ($(UNAME_S), Darwin)
LDFLAGS += -framework AVFoundation -framework CoreAudio -framework CoreVideo -framework CoreMedia -framework CoreGraphics
LDFLAGS += -framework VideoToolbox -framework AudioUnit -framework Foundation
LDFLAGS += -framework CoreFoundation -framework Security -framework SystemConfiguration
LDFLAGS += -lcompression -lresolv -lc++ -liconv
endif

ifeq ($(UNAME_S), Linux)
LDFLAGS += -lstdc++ -ldl -lrt -lresolv
endif

LDFLAGS += -lpthread

NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

# Serd — plain C, compiled with warnings silenced.
SERD_SRC = base64.c byte_source.c env.c n3.c node.c read_utf8.c reader.c \
           string.c system.c uri.c writer.c
SERD_OBJ = $(addprefix $(BUILD_DIR)/serd_, $(SERD_SRC:.c=.o))
SERD_CFLAGS = -std=c11 -w -I$(SERD_INC) -I$(SERD_SRC_DIR) -DSERD_STATIC
ifneq ($(filter ios-device ios-sim-arm64 ios-sim-x86_64,$(PLATFORM)),)
SERD_CFLAGS += $(IOS_FLAGS)
endif
ifeq ($(PLATFORM),android-arm64)
SERD_CFLAGS += $(ANDROID_FLAGS)
endif

# Library sources — mix of C++ (shim + internals that touch C++ state) and C.
LIB_CXX_SRC = carrier_jami.cc carrier_jami_signals.cc carrier_events.cc carrier_log.cc
LIB_CXX_OBJ = $(addprefix $(BUILD_DIR)/, $(LIB_CXX_SRC:.cc=.o))

# Plain-C library sources (linked into libcarrier.a alongside the C++ objects).
LIB_C_SRC = rdf_canon.c
LIB_C_OBJ = $(addprefix $(BUILD_DIR)/, $(LIB_C_SRC:.c=.o))

# CLI — still plain C (calls public C API only).
CLI_SRC = main_cli.c turtle_parse.c turtle_emit.c
CLI_OBJ = $(addprefix $(BUILD_DIR)/, $(CLI_SRC:.c=.o))

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

.PHONY: all clean distclean test install uninstall asan tsan deps libjami libjami-build libjami-fetch libcarrier-ios libcarrier-android

all: check-libjami $(BUILD_DIR)/libcarrier.a $(BUILD_DIR)/carrier-cli

# Cross-compile libcarrier.a for an iOS slice. Library-only — no carrier-cli,
# because the CLI links libjami + system frameworks and that link is host-
# specific (Darwin/Linux LDFLAGS in this Makefile). The .a is just an archive
# of carrier object files + serd objects; the antenna staticlib pulls them in
# and the final link happens in Xcode against per-slice libjami contribs.
#
# PLATFORM=ios-device     -> build-ios-device-arm64/libcarrier.a (arm64)
# PLATFORM=ios-simulator  -> build-ios-sim-fat/libcarrier.a (arm64 + x86_64 lipo'd)
#
# Headers are required ($(JAMI_INC) must exist; the C++ sources include
# <jami/*.h>) but the .a archives in $(JAMI_LIB) are not — no linking happens.
libcarrier-ios:
ifeq ($(PLATFORM),ios-device)
	@if [ ! -d "$(JAMI_INC)/jami" ]; then \
	    echo "ERROR: iOS libjami headers not found at $(JAMI_INC)/jami"; \
	    echo "       Run: make libjami PLATFORM=ios-device"; \
	    exit 1; \
	fi
	@$(MAKE) PLATFORM=ios-device $(BUILD_DIR)/libcarrier.a
else ifeq ($(PLATFORM),ios-simulator)
	@if [ ! -d "$(JAMI_INC)/jami" ]; then \
	    echo "ERROR: iOS libjami headers not found at $(JAMI_INC)/jami"; \
	    echo "       Run: make libjami PLATFORM=ios-simulator"; \
	    exit 1; \
	fi
	@$(MAKE) PLATFORM=ios-sim-arm64  build-ios-sim-arm64/libcarrier.a
	@$(MAKE) PLATFORM=ios-sim-x86_64 build-ios-sim-x86_64/libcarrier.a
	@mkdir -p build-ios-sim-fat
	@echo "  LIPO  build-ios-sim-fat/libcarrier.a"
	@lipo -create build-ios-sim-arm64/libcarrier.a build-ios-sim-x86_64/libcarrier.a \
	    -output build-ios-sim-fat/libcarrier.a
else
	@echo "ERROR: libcarrier-ios requires PLATFORM=ios-device|ios-simulator (got $(PLATFORM))" >&2
	@exit 1
endif

# Cross-compile libcarrier.a for the Android arm64 ABI. Library-only — no
# carrier-cli, same rationale as libcarrier-ios: the final link (libjami +
# system libs) happens later, here in antenna_ffi.so via build.rs, not in this
# Makefile. The .a is just carrier object files + serd objects compiled with
# the NDK clang. Headers must exist ($(JAMI_INC)/jami; the C++ sources include
# <jami/*.h>), but no contrib archives are linked.
#
# PLATFORM is forced to android-arm64 so the NDK toolchain + suffixes engage.
ANDROID_JAMI_INC := $(XDG_CACHE_HOME)/resonator/libjami/$(JAMI_SHA)-android-arm64/include
libcarrier-android:
	@if [ ! -d "$(ANDROID_JAMI_INC)/jami" ]; then \
	    echo "ERROR: Android libjami headers not found at $(ANDROID_JAMI_INC)/jami"; \
	    echo "       Run: make libjami PLATFORM=android-arm64"; \
	    exit 1; \
	fi
	@$(MAKE) PLATFORM=android-arm64 build-android-arm64/libcarrier.a

# `make all` builds carrier-cli, which is host-only — the link flags below
# target Darwin/Linux, not iOS. Calling `make all PLATFORM=ios-*` would
# resolve JAMI_LIB to the iOS prefix and then attempt a host link against
# iOS .a archives, which fails confusingly. Gate it here for a clear error.
check-libjami:
	@if [ "$(PLATFORM)" != "host" ]; then \
	    echo "ERROR: 'make all' / check-libjami is host-only (got PLATFORM=$(PLATFORM))."; \
	    echo "       For iOS prefixes, use:"; \
	    echo "         make libjami PLATFORM=ios-device     # iPhone arm64"; \
	    echo "         make libjami PLATFORM=ios-simulator  # Simulator fat"; \
	    exit 1; \
	fi
	@if [ -z "$(JAMI_SHA)" ]; then \
	    echo "ERROR: JAMI_VERSION is missing or empty (expected at $(CURDIR)/JAMI_VERSION)"; \
	    exit 1; \
	fi
	@if [ ! -f "$(JAMI_LIB)" ]; then \
	    echo "ERROR: libjami not found at $(JAMI_LIB)"; \
	    echo ""; \
	    echo "Populate the prefix with one of:"; \
	    echo "  make libjami          # fetch pre-built; fall back to source build"; \
	    echo "  make libjami-fetch    # fetch only (fail if not published)"; \
	    echo "  make libjami-build    # source build, 1-3 hours, hermetic"; \
	    echo ""; \
	    echo "Or set JAMI_PREFIX to point at an existing libjami install."; \
	    exit 1; \
	fi

libjami:
	@if ! tools/fetch-libjami.sh --platform=$(PLATFORM); then \
	    echo ""; \
	    echo "==> Fetch failed; falling back to source build."; \
	    tools/build-libjami.sh --platform=$(PLATFORM); \
	fi

libjami-build:
	@tools/build-libjami.sh --platform=$(PLATFORM)

libjami-fetch:
	@tools/fetch-libjami.sh --platform=$(PLATFORM)

deps: libjami

# --- Static library ---
$(BUILD_DIR)/libcarrier.a: $(LIB_CXX_OBJ) $(LIB_C_OBJ) $(SERD_OBJ)
	@echo "  AR    $(@F)"
	@$(AR) rcs $@ $^

# --- CLI binary ---
$(BUILD_DIR)/carrier-cli: $(CLI_OBJ) $(BUILD_DIR)/libcarrier.a
	@echo "  LD    $(@F)"
	@$(CXX) $(CXXFLAGS) -o $@ $(CLI_OBJ) $(BUILD_DIR)/libcarrier.a $(LDFLAGS)

# --- Compile rules ---
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC    $(<F)"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cc | $(BUILD_DIR)
	@echo "  CXX   $(<F)"
	@$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/serd_%.o: $(SERD_SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC    serd/$(<F)"
	@$(CC) $(SERD_CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# --- Tests ---
#
# Pure-C/C++ unit tests for helpers that don't pull in libjami. Each test
# source compiles with the matching compiler (C or C++), then the linker
# stitches them together — the same convention the main library uses.
# The libjami-binding side (carrier_jami*.cc) is covered by
# tests/two_peers.py, which spawns carrier-cli processes.
#
# test_turtle_emit.c is intentionally NOT listed: it predates the Jami
# migration and references the retired CarrierEvent shape. A v2 rewrite
# is its own follow-up; the current tests cover rdf_canon + vcard_utils,
# the surfaces touched by ISSUE-103 and ISSUE-104.

TEST_DIR        = tests
TEST_C_SRC      = test_main.c test_rdf_canon.c test_contact_restored.c
TEST_CXX_SRC    = test_vcard_utils.cc
TEST_C_OBJ      = $(addprefix $(BUILD_DIR)/$(TEST_DIR)/, $(TEST_C_SRC:.c=.o))
TEST_CXX_OBJ    = $(addprefix $(BUILD_DIR)/$(TEST_DIR)/, $(TEST_CXX_SRC:.cc=.o))
# turtle_emit.o is normally a CLI-only object (it depends on carrier.h's
# event union, not on libjami). test_contact_restored.c links against it
# directly so the C-side turtle wire shape is pinned by a unit test.
TEST_SUPPORT    = $(BUILD_DIR)/rdf_canon.o $(BUILD_DIR)/turtle_emit.o $(SERD_OBJ)

.PHONY: test test-archive test-saved
test: $(BUILD_DIR)/carrier-tests
	@echo "  RUN   carrier-tests"
	@$(BUILD_DIR)/carrier-tests

# Drives carrier-cli through the create / export / import / remove flows
# added for ISSUE-123 (Cut A). Pulls in libjami via a child process, so it
# only runs after the main `make` produced build/carrier-cli.
test-archive: $(BUILD_DIR)/carrier-cli
	@echo "  RUN   archive_round_trip.py"
	@python3 $(TEST_DIR)/archive_round_trip.py

# Exercises carrier:GetSavedConversation / carrier:SavedConversation —
# find-or-create the single-member-self swarm backing messenger2's
# "Saved Messages" workspace. End-to-end against libjami.
test-saved: $(BUILD_DIR)/carrier-cli
	@echo "  RUN   saved_conversation.py"
	@python3 $(TEST_DIR)/saved_conversation.py

$(BUILD_DIR)/carrier-tests: $(TEST_C_OBJ) $(TEST_CXX_OBJ) $(TEST_SUPPORT) | $(BUILD_DIR)
	@echo "  LD    $(@F)"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

$(BUILD_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.c | $(BUILD_DIR)/$(TEST_DIR)
	@echo "  CC    $(<F)"
	@$(CC) $(CFLAGS) -I$(TEST_DIR) -c -o $@ $<

$(BUILD_DIR)/$(TEST_DIR)/%.o: $(TEST_DIR)/%.cc | $(BUILD_DIR)/$(TEST_DIR)
	@echo "  CXX   $(<F)"
	@$(CXX) $(CXXFLAGS) -I$(TEST_DIR) -c -o $@ $<

$(BUILD_DIR)/$(TEST_DIR):
	@mkdir -p $@

# --- Sanitizers ---
asan: clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -fsanitize=address -fno-omit-frame-pointer -g" \
	        CFLAGS="$(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g" \
	        LDFLAGS="$(LDFLAGS) -fsanitize=address"

tsan: clean
	$(MAKE) CXXFLAGS="$(CXXFLAGS) -fsanitize=thread -g" \
	        CFLAGS="$(CFLAGS) -fsanitize=thread -g" \
	        LDFLAGS="$(LDFLAGS) -fsanitize=thread"

# --- Install ---
install: all
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 $(BUILD_DIR)/libcarrier.a $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(INC_DIR)/carrier.h $(DESTDIR)$(PREFIX)/include/
	install -m 755 $(BUILD_DIR)/carrier-cli $(DESTDIR)$(PREFIX)/bin/
	install -m 644 man/carrier-cli.1 $(DESTDIR)$(PREFIX)/share/man/man1/
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@VERSION@|3.0.0|g' \
	    carrier.pc.in > $(DESTDIR)$(PREFIX)/lib/pkgconfig/carrier.pc

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libcarrier.a
	rm -f $(DESTDIR)$(PREFIX)/include/carrier.h
	rm -f $(DESTDIR)$(PREFIX)/bin/carrier-cli
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/carrier.pc
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/carrier-cli.1

clean:
	rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/libcarrier.a $(BUILD_DIR)/carrier-cli

distclean: clean
	@echo "distclean: not removing $(JAMI_PREFIX) — run 'rm -rf $(XDG_CACHE_HOME)/resonator' to wipe the libjami cache"
