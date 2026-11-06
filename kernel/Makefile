# SPDX-License-Identifier: GPL-2.0
#
# Kbuild makefile for the ringchar character device.
#
# KDIR defaults to the headers for the running kernel. Override it to build
# against another tree, which is how the module is cross-built for a target
# board or compiled against a kernel other than the host's:
#
#   make KDIR=/lib/modules/6.8.0-100-generic/build
#   make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

obj-m += ringchar.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# -Wall is already on in kbuild; the extra flags catch the mistakes that
# actually bite in driver code -- a dropped __user annotation or an implicit
# narrowing of a size_t.
ccflags-y := -Wextra -Wno-unused-parameter -Wno-sign-compare

.PHONY: all modules clean check install load unload

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Sparse static analysis: the only tool that verifies the __user pointer
# annotations are respected across the copy boundary, which no compiler warning
# will catch.
check:
	$(MAKE) -C $(KDIR) M=$(PWD) C=2 CF="-D__CHECK_ENDIAN__" modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

load: modules
	sudo insmod ringchar.ko $(ARGS)

unload:
	sudo rmmod ringchar
