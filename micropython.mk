# QuickJS-NG module for MicroPython (Make-based ports: unix, stm32, rp2, ...)
#
# Usage:
#   make USER_C_MODULES=/path/to/this/directory ...
#
# (See https://docs.micropython.org/en/latest/develop/cmodules.html)

QJS_MOD_DIR := $(USERMOD_DIR)

# Python-facing module code (scanned for QSTRs / mp module definitions).
SRC_USERMOD_C += \
    $(QJS_MOD_DIR)/modquickjs.c

# QuickJS-NG engine sources.  Kept out of SRC_USERMOD_C so the QSTR scanner
# does not process vendored third-party code.
SRC_USERMOD_LIB_C += \
    $(QJS_MOD_DIR)/src/quickjs.c \
    $(QJS_MOD_DIR)/src/libregexp.c \
    $(QJS_MOD_DIR)/src/libunicode.c \
    $(QJS_MOD_DIR)/src/dtoa.c

CFLAGS_USERMOD += \
    -I$(QJS_MOD_DIR) \
    -I$(QJS_MOD_DIR)/src \
    -D_GNU_SOURCE

# Default QuickJS heap limit for Make-based (host) ports.
#
# modquickjs.c defaults the limit to 64 KiB (matching the original ESP32
# build), but on 64-bit hosts quickjs-ng needs more than 64 KiB just to
# create a JSContext (verified on the Unix port).  Override here so the
# module works out of the box on host ports; set QJS_DEFAULT_MEMORY_LIMIT
# on the make command line to change it.  CMake-based ports (esp32, rp2)
# are unaffected and keep the 64 KiB default.
QJS_DEFAULT_MEMORY_LIMIT ?= 8388608
CFLAGS_USERMOD += -DQUICKJS_DEFAULT_MEMORY_LIMIT=$(QJS_DEFAULT_MEMORY_LIMIT)

# The engine is vendored third-party code.  Ports such as unix compile with
# -Wall -Werror -Wextra; relax those diagnostics for the engine objects only.
# modquickjs.c itself is still compiled with the strict port flags.
_QJS_BUILD_DIR := $(BUILD)/$(notdir $(USERMOD_DIR))
$(_QJS_BUILD_DIR)/src/quickjs.o: CFLAGS +=\
    -Wno-format \
    -Wno-sign-compare \
    -Wno-unused-function \
    -Wno-unused-variable \
    -Wno-unused-parameter \
    -Wno-unused-but-set-variable \
    -Wno-maybe-uninitialized \
    -Wno-implicit-fallthrough \
    -Wno-array-bounds \
    -Wno-incompatible-pointer-types \
    -Wno-float-conversion \
    -Wno-double-promotion
$(_QJS_BUILD_DIR)/src/libregexp.o: CFLAGS +=\
    -Wno-sign-compare \
    -Wno-unused-function \
    -Wno-unused-variable \
    -Wno-unused-parameter \
    -Wno-unused-but-set-variable \
    -Wno-maybe-uninitialized \
    -Wno-implicit-fallthrough \
    -Wno-cast-function-type
$(_QJS_BUILD_DIR)/src/libunicode.o: CFLAGS +=\
    -Wno-sign-compare \
    -Wno-unused-function \
    -Wno-unused-variable \
    -Wno-unused-parameter \
    -Wno-unused-but-set-variable
$(_QJS_BUILD_DIR)/src/dtoa.o: CFLAGS +=\
    -Wno-sign-compare \
    -Wno-unused-function \
    -Wno-unused-variable \
    -Wno-unused-parameter \
    -Wno-maybe-uninitialized