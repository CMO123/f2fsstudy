#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>

#include "f2fs.h"

#ifdef AMF_PMU
int timeval_subtract (
	struct timeval *result,
	struct timeval *x, 
	struct timeval *y)
{   
	if (x->tv_usec < y->tv_usec) {
		int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
		y->tv_usec -= 1000000 * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_usec - y->tv_usec > 1000000) {
		int nsec = (x->tv_usec - y->tv_usec) / 1000000;
		y->tv_usec += 1000000 * nsec;
		y->tv_sec -= nsec;
	}

	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_usec = x->tv_usec - y->tv_usec;

	return x->tv_sec < y->tv_sec;
}


void amf_pmu_create (struct f2fs_sb_info *sbi)
{
pr_notice("Enter amf_pmu_create()\n");
	atomic64_set (&sbi->pmu.norm_r, 0);
	atomic64_set (&sbi->pmu.norm_w, 0);
	atomic64_set (&sbi->pmu.meta_r, 0);
	atomic64_set (&sbi->pmu.meta_w, 0);
	atomic64_set (&sbi->pmu.fs_gc_rw, 0);
	atomic64_set (&sbi->pmu.metalog_gc_rw, 0);
	atomic64_set (&sbi->pmu.mapping_w, 0);
	atomic64_set (&sbi->pmu.time_norm_r, 0);
	atomic64_set (&sbi->pmu.time_norm_w, 0);
	atomic64_set (&sbi->pmu.ckp_w, 0);
pr_notice("end amf_pmu_create()\n");
	do_gettimeofday (&sbi->pmu.time_start);
	/*return tv.tv_sec * 1000000 + tv.tv_usec;*/
}


void amf_pmu_display (struct f2fs_sb_info *sbi)
{
	struct timeval time_end, time_diff;

	do_gettimeofday (&time_end);
	timeval_subtract (&time_diff, &time_end, &sbi->pmu.time_start);

	printk (KERN_INFO "f2fs: -------------------------\n");
	printk (KERN_INFO "f2fs: -------------------------\n");
	printk (KERN_INFO "f2fs: < AMF-F2FS PERFORMANCE >\n");

	printk (KERN_INFO "f2fs: total reads: %ld\n", 
		atomic64_read (&sbi->pmu.norm_r) + 
		atomic64_read (&sbi->pmu.meta_r) +
		atomic64_read (&sbi->pmu.metalog_gc_rw));
	printk (KERN_INFO "f2fs: total writes: %ld\n", 
		atomic64_read (&sbi->pmu.norm_w) +
		atomic64_read (&sbi->pmu.meta_w) +
		atomic64_read (&sbi->pmu.metalog_gc_rw) +
		atomic64_read (&sbi->pmu.mapping_w));
	printk (KERN_INFO "f2fs: \n");

	printk (KERN_INFO "f2fs: normal reads: %ld\n", atomic64_read (&sbi->pmu.norm_r));
	printk (KERN_INFO "f2fs: normal writes: %ld\n", atomic64_read (&sbi->pmu.norm_w));
	printk (KERN_INFO "f2fs: \n");

	printk (KERN_INFO "f2fs: meta reads: %ld\n", atomic64_read (&sbi->pmu.meta_r));
	printk (KERN_INFO "f2fs: meta writes: %ld (cp: %ld)\n", atomic64_read (&sbi->pmu.meta_w), atomic64_read (&sbi->pmu.ckp_w));
	printk (KERN_INFO "f2fs: \n");

	printk (KERN_INFO "f2fs: gc fs copies: %ld\n", atomic64_read (&sbi->pmu.fs_gc_rw));
	printk (KERN_INFO "f2fs: gc metalog copies: %ld\n", atomic64_read (&sbi->pmu.metalog_gc_rw));
	printk (KERN_INFO "f2fs: \n");

	printk (KERN_INFO "f2fs: mapping writes: %ld\n", atomic64_read (&sbi->pmu.mapping_w));
	printk (KERN_INFO "f2fs: \n");

	printk (KERN_INFO "f2fs: Execution Time (secs): %lu.%lu\n", time_diff.tv_sec, time_diff.tv_usec);

	printk (KERN_INFO "f2fs: -------------------------\n");
}

#endif


