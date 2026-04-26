CC  ?= cc
CXX ?= c++
AR  ?= ar

PREFIX ?= /usr/local

# ---------------------------------------------------------------------------
# libjami prefix
#
# libjami is consumed as a pre-built static prefix at $(JAMI_PREFIX), which
# defaults to ${XDG_CACHE_HOME:-$HOME/.cache}/resonator/libjami/<sha>/ where
# <sha> is the line read from JAMI_VERSION. The prefix is laid out as:
#
#   $(JAMI_PREFIX)/lib/libjami-core.a   — the core library
#   $(JAMI_PREFIX)/lib/lib<contrib>.a   — contrib deps (~39 archives)
#   $(JAMI_PREFIX)/include/jami/*.h     — public headers
#
# Populate the prefix one of two ways:
#   make libjami-build    — clone jami-daemon at the pinned SHA outside the
#                           repo, build hermetically, stage into the cache
#                           (cold build: 1-3 hours)
#   make libjami-fetch    — download a pre-built tarball (future; not yet
#                           hosted)
#
# See arch/jami-migration.md D21 for the rationale (replaces D18 + D19).
# ---------------------------------------------------------------------------

JAMI_SHA          := $(shell tr -d '[:space:]' < JAMI_VERSION 2>/dev/null)
XDG_CACHE_HOME    ?= $(HOME)/.cache
JAMI_PREFIX       ?= $(XDG_CACHE_HOME)/resonator/libjami/$(JAMI_SHA)
JAMI_INC          = $(JAMI_PREFIX)/include
JAMI_LIB          = $(JAMI_PREFIX)/lib/libjami-core.a
JAMI_CONTRIB_LIB  = $(JAMI_PREFIX)/lib

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
UNAME_R := $(shell uname -r)

# pjsip names its libraries with the GNU arch convention (aarch64) while the
# host triple uses the Apple arch name (arm64). Only the lib filenames care.
ifeq ($(UNAME_S), Darwin)
JAMI_PJ_TRIPLE   = $(subst arm64,aarch64,$(UNAME_M))-apple-darwin$(UNAME_R)
else ifeq ($(UNAME_S), Linux)
JAMI_PJ_TRIPLE   = $(UNAME_M)-linux-gnu
endif

# --- Serd (compiled from submodule sources; still C) ---
SERD_DIR     = third_party/serd
SERD_INC     = $(SERD_DIR)/include
SERD_SRC_DIR = $(SERD_DIR)/src

SRC_DIR   = src
BUILD_DIR = build
INC_DIR   = include

# ---------------------------------------------------------------------------
# Compile flags
# ---------------------------------------------------------------------------

COMMON_WARN = -Wall -Wextra -Wno-unused-parameter -Wno-nullability-extension
COMMON_INC  = -I$(INC_DIR) -I$(SRC_DIR) -I$(JAMI_INC) -I$(SERD_INC)

CFLAGS   ?= -std=c11 -D_DEFAULT_SOURCE $(COMMON_WARN)
CFLAGS   += $(COMMON_INC) -DSERD_STATIC

CXXFLAGS ?= -std=c++20 $(COMMON_WARN)
CXXFLAGS += $(COMMON_INC)

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

# Library sources — mix of C++ (shim + internals that touch C++ state) and C.
LIB_CXX_SRC = carrier_jami.cc carrier_jami_signals.cc carrier_events.cc carrier_log.cc
LIB_CXX_OBJ = $(addprefix $(BUILD_DIR)/, $(LIB_CXX_SRC:.cc=.o))

# CLI — still plain C (calls public C API only).
CLI_SRC = main_cli.c turtle_parse.c turtle_emit.c
CLI_OBJ = $(addprefix $(BUILD_DIR)/, $(CLI_SRC:.c=.o))

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

.PHONY: all clean distclean test install uninstall asan tsan deps libjami-build libjami-fetch

all: check-libjami $(BUILD_DIR)/libcarrier.a $(BUILD_DIR)/carrier-cli

check-libjami:
	@if [ -z "$(JAMI_SHA)" ]; then \
	    echo "ERROR: JAMI_VERSION is missing or empty (expected at $(CURDIR)/JAMI_VERSION)"; \
	    exit 1; \
	fi
	@if [ ! -f "$(JAMI_LIB)" ]; then \
	    echo "ERROR: libjami not found at $(JAMI_LIB)"; \
	    echo ""; \
	    echo "Populate the prefix with one of:"; \
	    echo "  make libjami-build    # cold build, 1-3 hours, hermetic"; \
	    echo "  make libjami-fetch    # download pre-built tarball (not yet hosted)"; \
	    echo ""; \
	    echo "Or set JAMI_PREFIX to point at an existing libjami install."; \
	    exit 1; \
	fi

libjami-build:
	@tools/build-libjami.sh

libjami-fetch:
	@tools/fetch-libjami.sh

deps: libjami-build

# --- Static library ---
$(BUILD_DIR)/libcarrier.a: $(LIB_CXX_OBJ) $(SERD_OBJ)
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

# --- Tests (rewritten for v2 in a later task) ---
test:
	@echo "Tests need rewrite for Jami-backed carrier; see arch/jami-migration.md M2/M3."
	@exit 1

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
