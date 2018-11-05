#
# A shared Makefile from "https://www.kernel.org/doc/Documentation/kbuild/modules.txt"
# and optimized by myself
#

modname ?= f2fs
sourcelist ?= dir.o file.o inode.o namei.o hash.o super.o inline.o checkpoint.o gc.o data.o node.o segment.o recovery.o shrinker.o extent_cache.o sysfs.o debug.o xattr.o acl.o trace.o \
			 amf_pmu.o amf_ext.o tgt.o 
#headerdir ?=
KBUILD_EXTRA_SYMBOLS += /usr/src/kernels/linux/drivers/nvme/host/lightnvm/pblk-revise/Module.symvers
export KBUILD_EXTRA_SYMBOLS
EXTRA_CFLAGS += -DAMF_NO_SSR
EXTRA_CFLAGS += -DAMF_DEBUG_MSG
EXTRA_CFLAGS += -DAMF_SNAPSHOT
EXTRA_CFLAGS += -DAMF_META_LOGGING
EXTRA_CFLAGS += -DAMF_TRIM
#EXTRA_CFLAGS += -DRISA_LARGE_SEGMENT
EXTRA_CFLAGS += -DAMF_PMU
EXTRA_CFLAGS += -DAMF_DRAM_META_LOGGING
#EXTRA_CFLAGS += -DRISA_BETA	# For a larger segment support
#==========================================================
ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
obj-m  := $(modname).o
#ccflags-y := -I$(headerdir)
$(modname)-y := $(sourcelist)

else
# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD
	rm -rf modules.order .tmp_versions *.mod* *.o *.o.cmd .*.cmd
clean:
	rm -rf modules.order Module.symvers .tmp_versions *.ko* *.mod* *.o *.o.cmd .*.cmd 

#Module specific targets
hello:
	echo "hello"
endif
