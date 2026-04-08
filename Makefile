CC ?= cc
AR ?= ar

DEPS_DIR = ../deps
TOXCORE_INC = $(DEPS_DIR)/include
TOXCORE_LIB = $(DEPS_DIR)/lib

SERD_DIR = ../serd
SERD_INC = $(SERD_DIR)/include
SERD_SRC_DIR = $(SERD_DIR)/src

SRC_DIR = src
BUILD_DIR = build
INC_DIR = include

CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wno-unused-parameter
CFLAGS += -I$(INC_DIR) -I$(SRC_DIR) -I$(TOXCORE_INC) -I$(SERD_INC) -DSERD_STATIC

LDFLAGS ?=
LDFLAGS += -L$(TOXCORE_LIB) -ltoxcore
LDFLAGS += $(shell pkg-config --libs libsodium opus vpx 2>/dev/null || echo "-lsodium -lopus -lvpx")
LDFLAGS += -lpthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Linux)
LDFLAGS += -ldl -lrt
endif

# Serd objects (Turtle parser library)
SERD_SRC = base64.c byte_source.c env.c n3.c node.c read_utf8.c reader.c \
           string.c system.c uri.c writer.c
SERD_OBJ = $(addprefix $(BUILD_DIR)/serd_, $(SERD_SRC:.c=.o))
SERD_CFLAGS = -std=c11 -w -I$(SERD_INC) -I$(SERD_SRC_DIR) -DSERD_STATIC

# Library objects (no main)
LIB_SRC = carrier.c carrier_events.c
LIB_OBJ = $(addprefix $(BUILD_DIR)/, $(LIB_SRC:.c=.o))

# CLI objects
CLI_SRC = main_cli.c turtle_parse.c turtle_emit.c
CLI_OBJ = $(addprefix $(BUILD_DIR)/, $(CLI_SRC:.c=.o))

.PHONY: all clean

all: $(BUILD_DIR)/libcarrier.a $(BUILD_DIR)/carrier-cli

# Static library (includes serd)
$(BUILD_DIR)/libcarrier.a: $(LIB_OBJ) $(SERD_OBJ)
	@echo "  AR    $(@F)"
	@$(AR) rcs $@ $^

# CLI binary
$(BUILD_DIR)/carrier-cli: $(CLI_OBJ) $(BUILD_DIR)/libcarrier.a
	@echo "  LD    $(@F)"
	@$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(BUILD_DIR)/libcarrier.a $(LDFLAGS)

# Compile carrier sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC    $(<F)"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Compile serd sources
$(BUILD_DIR)/serd_%.o: $(SERD_SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC    serd/$(<F)"
	@$(CC) $(SERD_CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
