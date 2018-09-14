# Makefile for F2FS (for RISA)
#

TARGET = f2fs
SRCS = $(wildcard *.c)
OBJS = f2fs-y
MDIR = drivers/misc
CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)
DEST = /lib/modules/$(CURRENT)/kernel/$(MDIR)

#CONFIG_RISA = n
CONFIG_RISA = y
EXTRA_CFLAGS += -DRISA_NO_SSR
#EXTRA_CFLAGS += -DRISA_DEBUG_MSG
EXTRA_CFLAGS += -DRISA_SNAPSHOT
EXTRA_CFLAGS += -DRISA_META_LOGGING
EXTRA_CFLAGS += -DRISA_TRIM
#EXTRA_CFLAGS += -DRISA_LARGE_SEGMENT
EXTRA_CFLAGS += -DRISA_PMU
#EXTRA_CFLAGS += -DRISA_BETA	# For a larger segment support

# improvement for direct I/O taken from
# - https://www.mail-archive.com/linux-f2fs-devel@lists.sourceforge.net/msg00727.html
# - http://lkml.iu.edu/hypermail/linux/kernel/1311.2/01080.html
#EXTRA_CFLAGS += -DDIRECT_IO	

#EXTRA_CFLAGS += -DRISA_DRAM_META_LOGGING

obj-m		:= $(TARGET).o
module-objs	:= $(OBJS)
ccflags-y	+= -Wno-unused-function

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	-rm -f *.o .*.cmd .*.flags *.mod.c *.c~ *.h~
-include $(KDIR)/Rules.make

obj-$(CONFIG_F2FS_FS) += f2fs.o

f2fs-y		:= dir.o file.o inode.o namei.o hash.o super.o
f2fs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o 
f2fs-y		+= risa_pmu.o	# PMU
f2fs-$(CONFIG_RISA)		+= risa_ext.o risa_ext_dram.o # Extensions for RISA
f2fs-$(CONFIG_F2FS_STAT_FS) += debug.o
f2fs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
f2fs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
