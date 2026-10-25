# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -O3

# Upstream v1.10.0 is C11; the kernel's global default is gnu89.
ccflags-y += -std=gnu11
# vendored upstream coding style is not kernel style
ccflags-y += $(call cc-disable-warning, declaration-after-statement)

# upstream/ holds verbatim copies of the upstream LZ4 sources; do not edit
# them.  The three entry point files include them.  freestanding/ supplies
# the ISO C headers upstream includes that -nostdinc does not provide.
ccflags-y += -I $(srctree)/$(src)/freestanding

obj-$(CONFIG_LZ4_COMPRESS)	+= lz4_compress.o
obj-$(CONFIG_LZ4HC_COMPRESS)	+= lz4hc_compress.o
obj-$(CONFIG_LZ4_DECOMPRESS)	+= lz4_decompress.o

# lz4hc.c pulls in lz4.c's common definitions, which include two decoder
# tables nothing here uses when LZ4_FAST_DEC_LOOP is off.
CFLAGS_lz4hc_compress.o += $(call cc-disable-warning, unused-const-variable)
