# Out-of-tree kernel module Makefile for the LR2021 driver.
# Expects to run inside the pivid-builder image; the image's ENV provides:
#   ARCH=arm64
#   CROSS_COMPILE=aarch64-linux-gnu-
#   KDIR=/opt/rpi-linux       (kernel source w/ modules_prepare already done)
# Override KDIR= on the command line for ad-hoc builds against a different
# kernel tree.

KDIR ?= /opt/rpi-linux
PWD  := $(shell pwd)

.PHONY: modules clean

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
