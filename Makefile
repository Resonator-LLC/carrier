CC ?= cc
AR ?= ar

PREFIX ?= /usr/local

# --- Toxcore (built from submodule) ---
TOXCORE_DIR    = third_party/toxcore
TOXCORE_BUILD  = $(TOXCORE_DIR)/build
TOXCORE_PREFIX = $(CURDIR)/build/deps
TOXCORE_INC    = $(TOXCORE_PREFIX)/include
TOXCORE_LIB    = $(TOXCORE_PREFIX)/lib
TOXCORE_STAMP  = $(TOXCORE_PREFIX)/.built

# --- Serd (compiled from submodule sources) ---
SERD_DIR     = third_party/serd
SERD_INC     = $(SERD_DIR)/include
SERD_SRC_DIR = $(SERD_DIR)/src

SRC_DIR   = src
BUILD_DIR = build
INC_DIR   = include

CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wno-nullability-extension
CFLAGS += -I$(INC_DIR) -I$(SRC_DIR) -I$(TOXCORE_INC) -I$(SERD_INC) -DSERD_STATIC

LDFLAGS ?=
LDFLAGS += -L$(TOXCORE_LIB) -ltoxcore
LDFLAGS += $(shell pkg-config --libs libsodium opus vpx 2>/dev/null || echo "-lsodium -lopus -lvpx")
LDFLAGS += -lpthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Linux)
LDFLAGS += -ldl -lrt
endif

NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Serd objects (Turtle parser library)
SERD_SRC = base64.c byte_source.c env.c n3.c node.c read_utf8.c reader.c \
           string.c system.c uri.c writer.c
SERD_OBJ = $(addprefix $(BUILD_DIR)/serd_, $(SERD_SRC:.c=.o))
SERD_CFLAGS = -std=c11 -w -I$(SERD_INC) -I$(SERD_SRC_DIR) -DSERD_STATIC

# Library objects (no main)
LIB_SRC = carrier.c carrier_events.c carrier_log.c
LIB_OBJ = $(addprefix $(BUILD_DIR)/, $(LIB_SRC:.c=.o))

# CLI objects
CLI_SRC = main_cli.c turtle_parse.c turtle_emit.c
CLI_OBJ = $(addprefix $(BUILD_DIR)/, $(CLI_SRC:.c=.o))

# Test objects (linked against turtle_emit.o only — no toxcore needed)
TEST_SRC = $(wildcard tests/*.c)
TEST_OBJ = $(addprefix $(BUILD_DIR)/, $(notdir $(TEST_SRC:.c=.o)))

.PHONY: all clean distclean test install uninstall asan tsan deps

all: $(BUILD_DIR)/libcarrier.a $(BUILD_DIR)/carrier-cli

# --- Toxcore build ---

$(TOXCORE_STAMP): | $(BUILD_DIR)
	@echo "  CMAKE toxcore (this may take a minute)..."
	@mkdir -p $(TOXCORE_BUILD)
	@cd $(TOXCORE_BUILD) && cmake .. \
		-DCMAKE_INSTALL_PREFIX=$(TOXCORE_PREFIX) \
		-DBUILD_TOXAV=ON \
		-DMUST_BUILD_TOXAV=ON \
		-DENABLE_SHARED=OFF \
		-DENABLE_STATIC=ON \
		-DCMAKE_C_FLAGS="-w" \
		> /dev/null 2>&1
	@$(MAKE) -C $(TOXCORE_BUILD) -j$(NPROC) --no-print-directory > /dev/null 2>&1
	@$(MAKE) -C $(TOXCORE_BUILD) install --no-print-directory > /dev/null 2>&1
	@touch $@

deps: $(TOXCORE_STAMP)

# --- Static library (includes serd) ---

$(BUILD_DIR)/libcarrier.a: $(LIB_OBJ) $(SERD_OBJ)
	@echo "  AR    $(@F)"
	@$(AR) rcs $@ $^

# --- CLI binary ---

$(BUILD_DIR)/carrier-cli: $(CLI_OBJ) $(BUILD_DIR)/libcarrier.a
	@echo "  LD    $(@F)"
	@$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(BUILD_DIR)/libcarrier.a $(LDFLAGS)

# Carrier sources depend on toxcore headers
$(LIB_OBJ): $(TOXCORE_STAMP)
$(CLI_OBJ): $(TOXCORE_STAMP)

# Compile carrier sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC    $(<F)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Compile serd sources
$(BUILD_DIR)/serd_%.o: $(SERD_SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC    serd/$(<F)"
	@$(CC) $(SERD_CFLAGS) -c -o $@ $<

# Compile test sources
$(BUILD_DIR)/%.o: tests/%.c | $(BUILD_DIR)
	@echo "  CC    tests/$(<F)"
	@$(CC) $(CFLAGS) -Itests -I$(SRC_DIR) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# --- Test (no toxcore dependency) ---

$(BUILD_DIR)/test_carrier: $(TEST_OBJ) $(BUILD_DIR)/turtle_emit.o
	@echo "  LD    $(@F)"
	@$(CC) $(CFLAGS) -o $@ $(TEST_OBJ) $(BUILD_DIR)/turtle_emit.o

test: $(BUILD_DIR)/test_carrier
	@echo "  TEST  running tests..."
	@$(BUILD_DIR)/test_carrier

# --- Sanitizers ---

asan: clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address"

tsan: clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=thread -g" \
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
	    -e 's|@VERSION@|2.0.0|g' \
	    carrier.pc.in > $(DESTDIR)$(PREFIX)/lib/pkgconfig/carrier.pc

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libcarrier.a
	rm -f $(DESTDIR)$(PREFIX)/include/carrier.h
	rm -f $(DESTDIR)$(PREFIX)/bin/carrier-cli
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/carrier.pc
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/carrier-cli.1

clean:
	rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/libcarrier.a $(BUILD_DIR)/carrier-cli $(BUILD_DIR)/test_carrier

distclean:
	rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/libcarrier.a $(BUILD_DIR)/carrier-cli $(BUILD_DIR)/test_carrier
	rm -rf $(TOXCORE_BUILD) $(TOXCORE_PREFIX)
