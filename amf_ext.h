
#ifndef __AMF_EXT_H
#define __AMF_EXT_H

//注意一下3个参数要跟tools/mkfs/f2fs_format.c一样
#define NR_SUPERBLK_SECS	1
#define NR_MAPPING_SECS	3
#define NR_METALOGS_TIMES	2


#ifdef AMF_DEBUG_MSG
#define amf_msg(fmt, ...)	\
	do {	\
		printk(KERN_INFO fmt "\n", ##__VA_ARGS__);	\
	} while (0);
#define amf_dbg_msg(fmt, ...)	\
	do {	\
		printk(KERN_INFO fmt " (%d @ %s)\n", ##__VA_ARGS__, __LINE__, __FILE__);	\
	} while (0);
#else
#define amf_msg(fmt, ...)
#define amf_dbg_msg(fmt, ...)
#endif


