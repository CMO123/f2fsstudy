/*
 *	fs/f2fs/risa_ext.h
 * 
 *	Copyright (c) 2013 MIT CSAIL
 * 
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 **/

#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>

#include "f2fs.h"
#include "risa_ext.h"
#include "segment.h"


/* 
 * Handling read and write operations 
 */
static void risa_end_io_flash (struct bio *bio, int err)
{
	struct risa_bio_private *p = bio->bi_private;

	if (p->page) {
		ClearPageUptodate (p->page);
		unlock_page (p->page);
		__free_pages (p->page, 0);
	}

	if (p->is_sync)
		complete (p->wait);

	kfree (p);
	bio_put (bio);
}

static int32_t risa_readpage_flash (struct f2fs_sb_info *sbi, struct page* page, block_t blkaddr)
{
	struct block_device* bdev = sbi->sb->s_bdev;
	struct bio* bio = NULL;
	struct risa_bio_private* p = NULL;

	DECLARE_COMPLETION_ONSTACK (wait);

retry:
	p = kmalloc (sizeof (struct risa_bio_private), GFP_NOFS);
	if (!p) {
		cond_resched();
		goto retry;
	}

	/* allocate a new bio */
	bio = f2fs_bio_alloc (bdev, 1);

	/* initialize the bio */
	bio->bi_sector = SECTOR_FROM_BLOCK (sbi, blkaddr);
	bio->bi_end_io = risa_end_io_flash;

	p->sbi = sbi;
	p->page = NULL;
	bio->bi_private = p;

	/* put a bio into a bio queue */
	if (bio_add_page (bio, page, PAGE_CACHE_SIZE, 0) < PAGE_CACHE_SIZE) {
		risa_dbg_msg ("Error occur while calling risa_readpage");
		kfree (bio->bi_private);
		bio_put (bio);
		return -EFAULT;
	}

	/* submit a bio request to a device */
	p->is_sync = true;
	p->wait = &wait;
	submit_bio (READ_SYNC, bio);
	wait_for_completion (&wait);

	/* see if page is correct or not */
	if (PageError(page))
		return -EIO;

	return 0;
}

static int32_t risa_writepage_flash (struct f2fs_sb_info *sbi, struct page* page, block_t blkaddr, uint8_t sync)
{
	struct block_device* bdev = sbi->sb->s_bdev;
	struct bio* bio = NULL;
	struct risa_bio_private* p = NULL;

	DECLARE_COMPLETION_ONSTACK (wait);

retry:
	p = kmalloc (sizeof (struct risa_bio_private), GFP_NOFS);
	if (!p) {
		cond_resched();
		goto retry;
	}

	/* allocate a new bio */
	bio = f2fs_bio_alloc (bdev, 1);

	/* initialize the bio */
	bio->bi_sector = SECTOR_FROM_BLOCK (sbi, blkaddr);
	bio->bi_end_io = risa_end_io_flash;

	p->sbi = sbi;
	p->page = page;
	bio->bi_private = p;

	/* put a bio into a bio queue */
	if (bio_add_page (bio, page, PAGE_CACHE_SIZE, 0) < PAGE_CACHE_SIZE) {
		risa_dbg_msg ("Error occur while calling risa_readpage");
		kfree (bio->bi_private);
		bio_put (bio);
		return -EFAULT;
	}

	/* submit a bio request to a device */
	if (sync == 1) {
		p->is_sync = true;
		p->wait = &wait;
	} else {
		p->is_sync = false;
	}
	submit_bio (WRITE_FLUSH_FUA, bio);
	if (sync == 1) {
		wait_for_completion (&wait);
	}

	/* see if page is correct or not */
	if (PageError(page))
		return -EIO;

	return 0;
}

int8_t risa_readpage (struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr)
{
	int8_t ret;
#ifdef RISA_DRAM_META_LOGGING
	ret = risa_readpage_dram (sbi, page, pblkaddr);
#else
	down_read (&sbi->bio_sem);
	ret = risa_readpage_flash (sbi, page, pblkaddr);
	up_read (&sbi->bio_sem);
#endif
	return ret;
}

int8_t risa_writepage (
	struct f2fs_sb_info* sbi, 
	struct page* page, 
	block_t pblkaddr, 
	uint8_t sync)
{
	int8_t ret;
#ifdef RISA_DRAM_META_LOGGING
	ret = risa_writepage_dram (sbi, page, pblkaddr); 
#else
	down_write (&sbi->bio_sem);
	ret = risa_writepage_flash (sbi, page, pblkaddr, sync);
	up_write (&sbi->bio_sem);
#endif
	return ret;
}


/*
 * Create mapping & summary tables 
 */
static int32_t create_metalog_mapping_table (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	struct page* page = NULL;

	uint32_t i = 0, j = 0;
	uint8_t is_dead_section = 1;
	int32_t ret = 0;

	/* get the geometry information */
	ri->nr_mapping_phys_blks = NR_MAPPING_SECS * ri->blks_per_sec;
	ri->nr_mapping_logi_blks = ri->nr_metalog_logi_blks / 1020;
	if (ri->nr_metalog_logi_blks % 1020 != 0) {
		ri->nr_mapping_logi_blks++;
	}

	risa_msg ("--------------------------------");
	risa_msg (" # of mapping entries: %u", ri->nr_metalog_logi_blks);
	risa_msg (" * mapping table blkaddr: %u (blk)", ri->mapping_blkofs);
	risa_msg (" * mapping table length: %u (blk)", ri->nr_mapping_phys_blks);

	/* allocate the memory space for the summary table */
	if ((ri->map_blks = (struct risa_map_blk*)kmalloc (
			sizeof (struct risa_map_blk) * ri->nr_mapping_logi_blks, GFP_KERNEL)) == NULL) {
		risa_dbg_msg ("Errors occur while allocating memory space for the mapping table");
		goto out;
	}
	memset (ri->map_blks, 0x00, sizeof (struct risa_map_blk) * ri->nr_mapping_logi_blks);

	/* get the free page from the memory pool */
	page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (page)) {
		risa_dbg_msg ("Errors occur while allocating page");
		kfree (ri->map_blks);
		ret = PTR_ERR (page);
		goto out;
	}
	lock_page (page);

	/* read the mapping info from the disk */
	ri->mapping_gc_sblkofs = -1;
	ri->mapping_gc_eblkofs = -1;

	/* read the mapping info from the disk */
	for (i = 0; i < NR_MAPPING_SECS; i++) {
		is_dead_section = 1;

		for (j = 0; j < ri->blks_per_sec; j++) {
			__le32* ptr_page_addr = NULL;
			struct risa_map_blk* new_map_blk = NULL;

			/* read the mapping data from NAND devices */
			/*printk (KERN_INFO "MR: %lu\n", ri->mapping_blkofs + (i * ri->blks_per_sec) + j);*/
			down_read (&sbi->bio_sem);
			if (risa_readpage_flash (sbi, page, ri->mapping_blkofs + (i * ri->blks_per_sec) + j) != 0) {
				up_read (&sbi->bio_sem);
				risa_dbg_msg ("Errors occur while reading the mapping data from NAND devices");
				ret = -1;
				goto out;
			}
			up_read (&sbi->bio_sem);

			/* get the virtual address from the page */
			ptr_page_addr = (__le32*)page_address (page);
			new_map_blk = (struct risa_map_blk*)ptr_page_addr;

			/* check version # */
			if (new_map_blk->magic == cpu_to_le32 (0xEF)) {
				uint32_t index = le32_to_cpu (new_map_blk->index);
				/*risa_msg ("index: %u (old ver: %u, new ver: %u)", index, le32_to_cpu (ri->map_blks[index/1020].ver), le32_to_cpu (new_map_blk->ver));*/
				if (le32_to_cpu (ri->map_blks[index/1020].ver) <= le32_to_cpu (new_map_blk->ver)) {
					/*risa_msg ("copy new map blk");*/
					memcpy (&ri->map_blks[index/1020], ptr_page_addr, F2FS_BLKSIZE);
					is_dead_section = 0; /* this section has a valid blk */
				}
			}

			/* goto the next page */
			ClearPageUptodate (page);
		}

		/* is it dead? */
		if (is_dead_section == 1) {
			printk (KERN_INFO "dead section detected: %u\n", i);
			if (ri->mapping_gc_eblkofs == -1 && ri->mapping_gc_sblkofs == -1) {
				ri->mapping_gc_eblkofs = i * ri->blks_per_sec;
				ri->mapping_gc_sblkofs = i * ri->blks_per_sec + ri->blks_per_sec;
				ri->mapping_gc_sblkofs = ri->mapping_gc_sblkofs % ri->nr_mapping_phys_blks;
				risa_do_trim (sbi, ri->mapping_blkofs + ri->mapping_gc_eblkofs, ri->blks_per_sec); 
			}
		}
	}

	/* is there a free section for the mapping table? */
	if (ri->mapping_gc_sblkofs == -1 || ri->mapping_gc_eblkofs == -1) {
		risa_msg ("[ERROR] oops! there is no free space for the mapping table");
		ret = -1;
	} else {
		risa_msg ("-------------------------------");
		risa_msg ("ri->mapping_gc_slbkofs: %u (%u)", 
			ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
		risa_msg ("ri->mapping_gc_eblkofs: %u (%u)", 
			ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
		risa_msg ("-------------------------------");
	}

out:
	/* unlock & free the page */
	unlock_page (page);
	__free_pages (page, 0);

	return ret;
}

static int32_t create_metalog_summary_table (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	uint32_t sum_length = 0;
	uint32_t i = 0, j = 0;
	uint8_t is_dead = 1;
	int32_t ret = 0;

	/* get the geometry information */
	sum_length = (sizeof (uint8_t) * ri->nr_metalog_phys_blks + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE;

	risa_msg ("--------------------------------");
	risa_msg (" * summary table length: %u", sum_length);
	risa_msg ("--------------------------------");

	/* allocate the memory space for the summary table */
	if ((ri->summary_table = 
			(uint8_t*)kmalloc (sum_length * F2FS_BLKSIZE, GFP_KERNEL)) == NULL) {
		risa_dbg_msg ("Errors occur while allocating memory space for the mapping table");
		ret = -1;
		goto out;
	}

	/* set all the entries of the summary table invalid */
	memset (ri->summary_table, 2, sum_length * F2FS_BLKSIZE);

	/* set the entries which are vailid in the mapping valid */
	for (i = 0; i < ri->nr_mapping_logi_blks; i++) {
		for (j = 0; j < 1020; j++) {
			__le32 phyofs = ri->map_blks[i].mapping[j];
			if (le32_to_cpu (phyofs) != -1) {
				/*risa_msg ("summary: set phyofs %u to valid", le32_to_cpu (phyofs) - ri->metalog_blkofs);*/
				ri->summary_table[le32_to_cpu (phyofs) - ri->metalog_blkofs] = 1;
			}
		}
	}

	/* search for a section that contains only invalid blks */
	for (i = 0; i < ri->nr_metalog_phys_blks / ri->blks_per_sec; i++) {
		is_dead = 1;
		for (j = 0; j < ri->blks_per_sec; j++) {
			if (ri->summary_table[i*ri->blks_per_sec+j] != 2) {
				is_dead = 0;
				break;
			}
		}
		if (is_dead == 1) {
			ri->metalog_gc_eblkofs = i * ri->blks_per_sec;
			ri->metalog_gc_sblkofs = i * ri->blks_per_sec;
			ri->metalog_gc_sblkofs = (ri->metalog_gc_sblkofs + ri->blks_per_sec) % ri->nr_metalog_phys_blks;

			risa_do_trim (sbi, ri->mapping_blkofs + ri->metalog_gc_eblkofs, ri->blks_per_sec); 
			memset (&ri->summary_table[i*ri->blks_per_sec], 0x00, ri->blks_per_sec);
			break;
		}
	}

	/* metalog must have at least one dead section */
	if (is_dead == 0) {
		risa_msg ("[ERROR] oops! cannot find dead sections in metalog");
		ret = -1;
	} else {
		risa_msg ("-------------------------------");
		risa_msg ("ri->metalog_gc_slbkofs: %u (%u)", 
			ri->metalog_gc_sblkofs, ri->metalog_blkofs + ri->metalog_gc_sblkofs);
		risa_msg ("ri->metalog_gc_eblkofs: %u (%u)", 
			ri->metalog_gc_eblkofs, ri->metalog_blkofs + ri->metalog_gc_eblkofs);
		risa_msg ("-------------------------------");
	}

out:
	return ret;
}


static void destroy_metalog_summary_table (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	if (ri->summary_table) {
		kfree (ri->summary_table);
		ri->summary_table = NULL;
	}
}

static void destroy_metalog_mapping_table (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	if (ri->map_blks) {
		kfree (ri->map_blks);
		ri->map_blks = NULL;
	}
}

static void destroy_ri (struct f2fs_sb_info* sbi)
{
	if (sbi->ri) {
		kfree (sbi->ri);
		sbi->ri = NULL;
	}
}


/* 
 * create the structure for RISA (ri) 
 */
int32_t risa_create_ri (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = NULL;
	uint32_t nr_logi_metalog_segments = 0;
	uint32_t nr_phys_metalog_segments = 0;

	/* create risa_info structure */
	if ((ri = (struct risa_info*)kmalloc (
			sizeof (struct risa_info), GFP_KERNEL)) == NULL) {
		risa_dbg_msg ("Errors occur while creating risa_info");
		return -1;
	}
	sbi->ri = ri;

	/* initialize some variables */
	ri->mapping_blkofs = get_mapping_blkofs (sbi);
	ri->metalog_blkofs = get_metalog_blkofs (sbi);

	nr_logi_metalog_segments = get_nr_logi_meta_segments (sbi);
	nr_phys_metalog_segments = get_nr_phys_meta_segments (sbi, nr_logi_metalog_segments);

	ri->nr_metalog_logi_blks = SEGS2BLKS (sbi, nr_logi_metalog_segments);
	ri->nr_metalog_phys_blks = SEGS2BLKS (sbi, nr_phys_metalog_segments);

	ri->blks_per_sec = sbi->segs_per_sec * (1 << sbi->log_blocks_per_seg);

	/* create mutex for GC */
	mutex_init(&ri->risa_gc_mutex);

	/* display information about metalog */
	risa_msg ("--------------------------------");
	risa_msg (" * mapping_blkofs: %u", ri->mapping_blkofs);
	risa_msg (" * metalog_blkofs: %u", ri->metalog_blkofs);
	risa_msg (" * # of blks per sec: %u", ri->blks_per_sec);
	risa_msg (" * # of logical meta-log blks: %u", ri->nr_metalog_logi_blks);
	risa_msg (" * # of physical meta-log blks: %u", ri->nr_metalog_phys_blks);
	risa_msg (" * the range of logical meta address: %u - %u", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_logi_blks);
	risa_msg (" * the range of physical meta address: %u - %u", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_phys_blks);

	return 0;
}

int32_t risa_build_ri (struct f2fs_sb_info *sbi)
{
	/* see if ri is initialized or not */
	if (sbi == NULL || sbi->ri == NULL) {
		risa_dbg_msg ("Error occur because some input parameters are NULL");
		return -1;
	}

	/* build meta-log mapping table */
	if (create_metalog_mapping_table (sbi) != 0) {
		risa_dbg_msg ("Errors occur while creating the metalog mapping table");
		goto error_metalog_mapping;
	}

	/* build meta-log summary table */
	if (create_metalog_summary_table (sbi) != 0) {
		risa_dbg_msg ("Errors occur while creating the metalog summary table");
		goto error_metalog_summary;
	}

#ifdef RISA_DRAM_META_LOGGING
	if (create_dram_metalog (sbi) != 0) {
		risa_dbg_msg ("Errors occur while creating the dram metalog");
		goto error_create_dram_metalog;
	}

	if (build_dram_metalog (sbi) != 0) {
		risa_dbg_msg ("Errors occur while building the dram metalog");
		goto error_build_dram_metalog;
	}
#endif

	return 0;

#ifdef RISA_DRAM_META_LOGGING
error_build_dram_metalog:
	destroy_dram_metalog (sbi);

error_create_dram_metalog:
	destroy_metalog_summary_table (sbi);
#endif

error_metalog_summary:
	destroy_metalog_mapping_table (sbi);

error_metalog_mapping:

	return -1;
}

void risa_destory_ri (struct f2fs_sb_info* sbi)
{
#ifdef RISA_DRAM_META_LOGGING
	destroy_dram_metalog (sbi);
#endif
	destroy_metalog_summary_table (sbi);
	destroy_metalog_mapping_table (sbi);
	destroy_ri (sbi);
}


/*
 * mapping table management 
 */
int32_t get_mapping_free_blks (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	uint32_t nr_free_blks;

	if (ri->mapping_gc_sblkofs < ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->nr_mapping_phys_blks - ri->mapping_gc_eblkofs + ri->mapping_gc_sblkofs;
	} else if (ri->mapping_gc_sblkofs > ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->mapping_gc_sblkofs - ri->mapping_gc_eblkofs;
	} else {
		risa_dbg_msg ("[ERROR] 'ri->mapping_gc_sblkofs (%u)' is equal to 'ri->mapping_gc_eblkofs (%u)'", 
			ri->mapping_gc_sblkofs, ri->mapping_gc_eblkofs);
		nr_free_blks = -1;
	}

	return nr_free_blks;
}

int8_t is_mapping_gc_needed (struct f2fs_sb_info* sbi, int32_t nr_free_blks)
{
	if (nr_free_blks <= (sbi->segs_per_sec * sbi->blocks_per_seg)) {
		return 0;
	}
	return -1;
}

int8_t risa_do_mapping_gc (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);

	/*
	risa_dbg_msg ("before gc");
	risa_msg ("-------------------------------");
	risa_msg ("ri->mapping_gc_slbkofs: %u (%u)", 
		ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
	risa_msg ("ri->mapping_gc_eblkofs: %u (%u)", 
		ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
	risa_msg ("-------------------------------");
	*/

	/* perform gc */
	risa_do_trim (sbi, ri->mapping_blkofs + ri->mapping_gc_sblkofs, ri->blks_per_sec); 

	/* advance 'mapping_gc_sblkofs' */
	ri->mapping_gc_sblkofs = (ri->mapping_gc_sblkofs + ri->blks_per_sec) % 
		ri->nr_mapping_phys_blks;

	/*
	risa_dbg_msg ("after gc");
	risa_msg ("-------------------------------");
	risa_msg ("ri->mapping_gc_slbkofs: %u (%u)", 
		ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
	risa_msg ("ri->mapping_gc_eblkofs: %u (%u)", 
		ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
	risa_msg ("-------------------------------");
	*/

	return 0;
}

int32_t risa_write_mapping_entries (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	struct page* page = NULL;
	int32_t nr_free_blks = 0;
	uint32_t i = 0;

	/* see if gc is needed for the mapping area */
	nr_free_blks = get_mapping_free_blks (sbi);
	if (is_mapping_gc_needed (sbi, nr_free_blks) == 0) {
		risa_do_mapping_gc (sbi);
	}

	/* TODO: see if there are any dirty mapping entries */

	/* write dirty entries to the mapping area */
	for (i = 0; i < ri->nr_mapping_logi_blks; i++) {
		__le32* ptr_page_addr = NULL;
		uint32_t version = 0;

		/* see if it is dirty or not */
		if (ri->map_blks[i].dirty == 0) {
			continue;
		}

		/* increase version numbers */
		version = le32_to_cpu (ri->map_blks[i].ver) + 1;
		ri->map_blks[i].ver = cpu_to_le32 (version);
		ri->map_blks[i].dirty = cpu_to_le32 (0);

		/* get the free page from the memory pool */
		page = alloc_page (GFP_NOFS | __GFP_ZERO);
		if (IS_ERR (page)) {
			risa_dbg_msg ("Errors occur while allocating a new page");
			return PTR_ERR (page);
		}
		lock_page (page);

		/* write dirty entires to NAND flash */
		ptr_page_addr = (__le32*)page_address (page);
		memcpy (ptr_page_addr, &ri->map_blks[i], F2FS_BLKSIZE);
		/*risa_dbg_msg ("writing mapping (%u): %u", */
		/*ri->map_blks[i].index, ri->mapping_blkofs + ri->mapping_gc_eblkofs);*/
		/*risa_writepage_flash (sbi, page, ri->mapping_blkofs + ri->mapping_gc_eblkofs, 1);*/
		risa_writepage_flash (sbi, page, ri->mapping_blkofs + ri->mapping_gc_eblkofs, 0);

		/* update physical location */
		ri->mapping_gc_eblkofs = 
			(ri->mapping_gc_eblkofs + 1) % ri->nr_mapping_phys_blks;

		atomic64_add (1, &sbi->pmu.mapping_w);
	}

	return 0;
}


/*
 * metalog management 
 */
int32_t is_valid_meta_lblkaddr (struct f2fs_sb_info* sbi, 
	block_t lblkaddr)
{
	struct risa_info* ri = RISA_RI (sbi);

	if (sbi->ri == NULL)
		return -1;
	
	if (lblkaddr >= ri->metalog_blkofs &&
		lblkaddr < ri->metalog_blkofs + ri->nr_metalog_logi_blks)
		return 0;

	return -1;
}

int32_t is_valid_meta_pblkaddr (struct f2fs_sb_info* sbi, block_t pblkaddr)
{
	struct risa_info* ri = RISA_RI (sbi);

	if (sbi->ri == NULL)
		return -1;
	
	if (pblkaddr >= ri->metalog_blkofs &&
		pblkaddr < ri->metalog_blkofs + ri->nr_metalog_phys_blks)
		return 0;

	return -1;
}

int32_t get_metalog_free_blks (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	uint32_t nr_free_blks;

	if (ri->metalog_gc_sblkofs < ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->nr_metalog_phys_blks - ri->metalog_gc_eblkofs + ri->metalog_gc_sblkofs;
	} else if (ri->metalog_gc_sblkofs > ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->metalog_gc_sblkofs - ri->metalog_gc_eblkofs;
	} else {
		risa_dbg_msg ("[ERROR] 'ri->metalog_gc_sblkofs (%u)' is equal to 'ri->metalog_gc_eblkofs (%u)'", 
			ri->metalog_gc_sblkofs, ri->metalog_gc_eblkofs);
		nr_free_blks = -1;
	}

	return nr_free_blks;
}

int8_t is_gc_needed (struct f2fs_sb_info* sbi, int32_t nr_free_blks)
{
	struct risa_info* ri = RISA_RI (sbi);

	mutex_lock (&ri->risa_gc_mutex); 
	if (nr_free_blks <= (sbi->segs_per_sec * sbi->blocks_per_seg)) {
		mutex_unlock (&ri->risa_gc_mutex); 
		return 0;
	}

	mutex_unlock (&ri->risa_gc_mutex); 
	return -1;
}

uint32_t risa_get_mapped_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr)
{
	struct risa_info* ri = RISA_RI (sbi);
	block_t pblkaddr;
	block_t new_lblkaddr;

	/* see if ri is initialized or not */
	if (sbi->ri == NULL)
		return NULL_ADDR;

	/* get the physical blkaddr from the mapping table */
	new_lblkaddr = lblkaddr - ri->metalog_blkofs;
	pblkaddr = le32_to_cpu (ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020]);
	if (pblkaddr == -1)
		pblkaddr = 0;

	/* see if 'pblkaddr' is valid or not */
	if (is_valid_meta_pblkaddr (sbi, pblkaddr) == -1) {
		if (pblkaddr != NULL_ADDR) {
			risa_msg ("invalid pblkaddr: (%llu (=%llu-%llu) => %u)", 
				(int64_t)lblkaddr - (int64_t)ri->metalog_blkofs,
				(int64_t)lblkaddr, 
				(int64_t)ri->metalog_blkofs, 
				pblkaddr);
		}
		return NULL_ADDR;
	}

	/* see if the summary table is correct or not */
	if (ri->summary_table[pblkaddr - ri->metalog_blkofs] == 0 ||
		ri->summary_table[pblkaddr - ri->metalog_blkofs] == 2) {
		risa_dbg_msg ("the summary table is incorrect: pblkaddr=%u (%u)",
			pblkaddr, ri->summary_table[pblkaddr - ri->metalog_blkofs]);
	}

	return pblkaddr;
}

uint32_t risa_get_new_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr, uint32_t length)
{
	struct risa_info* ri = RISA_RI (sbi);
	block_t pblkaddr = NULL_ADDR;

	/* see if ri is initialized or not */
	if (sbi->ri == NULL)
		return NULL_ADDR;

	/* have sufficent free blks - go ahead */
	if (ri->summary_table[ri->metalog_gc_eblkofs] == 0) {
		/* get the physical blkoff */
		pblkaddr = ri->metalog_blkofs + ri->metalog_gc_eblkofs;

		/* see if pblk is valid or not */
		if (is_valid_meta_pblkaddr (sbi, pblkaddr) == -1) {
			risa_dbg_msg ("pblkaddr is invalid (%u)", pblkaddr);
			return NULL_ADDR;
		}
	} else {
		risa_dbg_msg ("metalog_gc_eblkofs is NOT free: summary_table[%u] = %u",
			ri->metalog_gc_eblkofs, ri->summary_table[ri->metalog_gc_eblkofs]);
		return NULL_ADDR;
	}

	return pblkaddr;
}

int8_t risa_map_l2p (struct f2fs_sb_info* sbi, block_t lblkaddr, block_t pblkaddr, uint32_t length)
{
	struct risa_info* ri = RISA_RI (sbi);
	block_t cur_lblkaddr = lblkaddr;
	block_t cur_pblkaddr = pblkaddr;
	block_t new_lblkaddr;
	uint32_t loop = 0;

	/* see if ri is initialized or not */
	if (sbi->ri == NULL)
		return -1;

	/* see if pblkaddr is valid or not */
	if (pblkaddr == NULL_ADDR)
		return -1;

	for (loop = 0; loop < length; loop++) {
		block_t prev_pblkaddr = NULL_ADDR;

		/* see if cur_lblkaddr is valid or not */
		if (is_valid_meta_lblkaddr (sbi, cur_lblkaddr) == -1) {
			risa_dbg_msg ("is_valid_meta_lblkaddr is failed (cur_lblkaddr: %u)", cur_lblkaddr);
			return -1;
		}

		/* get the new pblkaddr */
		if (cur_pblkaddr == NULL_ADDR) {
			if ((cur_pblkaddr = risa_get_new_pblkaddr (sbi, cur_lblkaddr, length)) == NULL_ADDR) {
				risa_dbg_msg ("cannot get the new free block (cur_lblkaddr: %u)", cur_lblkaddr);
				return -1;
			} 
		}

		/* get the old pblkaddr */
		new_lblkaddr = cur_lblkaddr - ri->metalog_blkofs;
		prev_pblkaddr = le32_to_cpu (ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020]);
		if (prev_pblkaddr == -1)
			prev_pblkaddr = 0;

		/* see if 'prev_pblkaddr' is valid or not */
		if (is_valid_meta_pblkaddr (sbi, prev_pblkaddr) == 0) {
			/* make the entry of the summary table invalid */
			ri->summary_table[prev_pblkaddr - ri->metalog_blkofs] = 2;	/* set to invalid */

			/* trim */
			if (risa_do_trim (sbi, prev_pblkaddr, 1) == -1) {
				risa_dbg_msg (KERN_INFO "Errors occur while trimming the page during risa_map_l2p");
			}
		} else if (prev_pblkaddr != NULL_ADDR) {
			risa_dbg_msg ("invalid prev_pblkaddr = %llu", (int64_t)prev_pblkaddr);
		} else {
			/* it is porible that 'prev_pblkaddr' is invalid */
		}

		/* update the mapping & summary table */
		new_lblkaddr = cur_lblkaddr - ri->metalog_blkofs;
		ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020] = cpu_to_le32 (cur_pblkaddr);
		ri->map_blks[new_lblkaddr/1020].dirty = 1;

		ri->summary_table[cur_pblkaddr - ri->metalog_blkofs] = 1; /* set to valid */

		/* adjust end_blkofs in the meta-log */
		ri->metalog_gc_eblkofs = (ri->metalog_gc_eblkofs + 1) % (ri->nr_metalog_phys_blks);

		/* go to the next logical blkaddr */
		cur_lblkaddr++;
		cur_pblkaddr = NULL_ADDR;
	}

	return 0;
}

int8_t risa_do_trim (struct f2fs_sb_info* sbi, block_t pblkaddr, uint32_t nr_blks)
{
	if (test_opt (sbi, DISCARD)) {
		blkdev_issue_discard (
			sbi->sb->s_bdev, 
			SECTOR_FROM_BLOCK (sbi, pblkaddr), 
			nr_blks * 8, 
			GFP_NOFS, 
			0);
		return 0;
	}
	return -1;
}

int8_t risa_do_gc (struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	struct page* page = NULL;

	uint32_t cur_blkofs = 0;
	uint32_t loop = 0;

	mutex_lock (&ri->risa_gc_mutex); 

	/* see if ri is initialized or not */
	if (sbi->ri == NULL) {
		mutex_unlock(&ri->risa_gc_mutex); 
		return -1;
	}

	/* check the alignment */
	if (ri->metalog_gc_sblkofs % (sbi->segs_per_sec * sbi->blocks_per_seg) != 0) {
		risa_dbg_msg ("ri->metalog_gc_sblkofs %% sbi->blocks_per_seg != 0 (%u)", 
			ri->metalog_gc_sblkofs % (sbi->segs_per_sec * sbi->blocks_per_seg));
		mutex_unlock(&ri->risa_gc_mutex); 
		return -1;
	}

	/* read all valid blks in the victim segment */
	for (cur_blkofs = ri->metalog_gc_sblkofs; 
		 cur_blkofs < ri->metalog_gc_sblkofs + (sbi->segs_per_sec * sbi->blocks_per_seg); 
		 cur_blkofs++) 
	{
		uint8_t is_mapped = 0;
		uint32_t src_pblkaddr;
		uint32_t dst_pblkaddr;

		/* see if the block is valid or not */
		if (ri->summary_table[cur_blkofs] == 0 || ri->summary_table[cur_blkofs] == 2) {
			/* go to the next blks */
			ri->summary_table[cur_blkofs] = cpu_to_le32 (0); /* set to free */

			/* trim all the valid pages */
			if (risa_do_trim (sbi, ri->metalog_blkofs + cur_blkofs, 1) == -1) {
				risa_dbg_msg (KERN_INFO "Errors occur while trimming the page during GC");
			}
			continue;
		}

		/* allocate a new page (This page is released later by 'risa_end_io_flash' */
		page = alloc_page (GFP_NOFS | __GFP_ZERO);
		if (IS_ERR (page)) {
			risa_dbg_msg ("page is invalid");
			mutex_unlock(&ri->risa_gc_mutex); 
			return PTR_ERR (page);
		}
		lock_page (page);

		/* determine src & dst blks */
		src_pblkaddr = ri->metalog_blkofs + cur_blkofs;
		dst_pblkaddr = ri->metalog_blkofs + ri->metalog_gc_eblkofs;

		/* read the valid blk into the page */
		if (risa_readpage (sbi, page, src_pblkaddr) == -1) {
			printk (KERN_INFO "[ERROR] errors occur while reading the page during GC");
			continue;
		}

		/* write the valid pages into the free segment */
		if (risa_writepage (sbi, page, dst_pblkaddr, 0) == -1) {
			printk (KERN_INFO "[ERROR] errors occur while writing the page during GC");
			continue;
		}

		/* trim the source page */
		if (risa_do_trim (sbi, src_pblkaddr, 1) == -1) {
			printk (KERN_INFO "[ERROR] errors occur while trimming the page during GC");
			continue;
		}

		/* update mapping table */
		for (loop = 0; loop < ri->nr_metalog_logi_blks; loop++) {
			if (ri->map_blks[loop/1020].mapping[loop%1020] == src_pblkaddr) {
				ri->map_blks[loop/1020].mapping[loop%1020] = dst_pblkaddr;
				ri->map_blks[loop/1020].dirty = 1;
				is_mapped = 1;
				break;
			}
		}

		if (is_mapped != 1) {
			printk (KERN_INFO "[ERROR] cannot find a mapped physical blk");
		}

		/* update summary table */
		ri->summary_table[src_pblkaddr - ri->metalog_blkofs] = cpu_to_le32 (0); /* set to free */
		ri->summary_table[dst_pblkaddr - ri->metalog_blkofs] = cpu_to_le32 (1); /* see to valid */

		/* update end offset */
		ri->metalog_gc_eblkofs = (ri->metalog_gc_eblkofs + 1) % ri->nr_metalog_phys_blks;

#ifdef RISA_PMU
		atomic64_add (1, &sbi->pmu.metalog_gc_rw);
#endif
	}

	/* update start offset */
	ri->metalog_gc_sblkofs = 
		(ri->metalog_gc_sblkofs + (sbi->segs_per_sec * sbi->blocks_per_seg)) % 
		ri->nr_metalog_phys_blks;

	mutex_unlock (&ri->risa_gc_mutex); 

	return 0;
}

void risa_submit_bio_w (struct f2fs_sb_info* sbi, struct bio* bio, uint8_t sync)
{
	struct page* src_page = NULL;
	struct page* dst_page = NULL;
	struct bio_vec *bvec = NULL;

	uint8_t* src_page_addr = NULL;
	uint8_t* dst_page_addr = NULL;
	uint32_t bioloop = 0;
	int8_t ret = 0;

	bio_for_each_segment (bvec, bio, bioloop) {
		uint32_t pblkaddr = NULL_ADDR;
		uint32_t lblkaddr = NULL_ADDR;

		/* allocate a new page (This page is released later by 'risa_end_io_flash' */
		dst_page = alloc_page (GFP_NOFS | __GFP_ZERO);
		if (IS_ERR (dst_page)) {
			risa_dbg_msg ("Errors occur while allocating page");
			bio->bi_end_io (bio, -1);
			return;
		}
		lock_page (dst_page);

		/* check error cases */
		if (bvec == NULL || bvec->bv_len == 0 || bvec->bv_page == NULL)  {
			risa_dbg_msg ("bvec is wrong");
			break;
		}

		/* get the soruce page */
		src_page = bvec->bv_page;

		/* get the new pblkaddr */
		spin_lock (&sbi->mapping_lock);
		lblkaddr = src_page->index;
		pblkaddr = risa_get_new_pblkaddr (sbi, lblkaddr, 1);
		if (pblkaddr == NULL_ADDR) {
			spin_unlock (&sbi->mapping_lock);
			risa_dbg_msg ("risa_get_new_pblkaddr failed");
			ret = -1;
			goto out;
		}
		/* update mapping table */
		if (risa_map_l2p (sbi, lblkaddr, pblkaddr, 1) != 0) {
			spin_unlock (&sbi->mapping_lock);
			risa_dbg_msg ("risa_map_l2p failed");
			ret = -1;
			goto out;
		}
		spin_unlock (&sbi->mapping_lock);

		/* write the requested page */
		src_page_addr = (uint8_t*)page_address (src_page);
		dst_page_addr = (uint8_t*)page_address (dst_page);
		memcpy (dst_page_addr, src_page_addr, PAGE_CACHE_SIZE);

		if (risa_writepage_flash (sbi, dst_page, pblkaddr, sync) != 0) {
			risa_dbg_msg ("risa_write_page_flash failed");
			ret = -1;
			goto out;
		}
	}

out:
	if (ret == 0) {
		set_bit (BIO_UPTODATE, &bio->bi_flags);
		bio->bi_end_io (bio, 0);
	} else {
		bio->bi_end_io (bio, ret);
	}
}

void risa_submit_bio_r (struct f2fs_sb_info* sbi, struct bio* bio)
{
	struct page* src_page = NULL;
	struct page* dst_page = NULL;
	struct bio_vec *bvec = NULL;

	uint8_t* src_page_addr = NULL;
	uint8_t* dst_page_addr = NULL;
	uint32_t bioloop = 0;
	int8_t ret = 0;

	src_page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (src_page)) {
		risa_dbg_msg ("Errors occur while allocating page");
		bio->bi_end_io (bio, -1);
		return;
	}
	lock_page (src_page);

	bio_for_each_segment (bvec, bio, bioloop) {
		uint32_t pblkaddr = NULL_ADDR;
		uint32_t lblkaddr = NULL_ADDR;

		/* check error cases */
		if (bvec == NULL || bvec->bv_len == 0 || bvec->bv_page == NULL)  {
			risa_dbg_msg ("bvec is wrong");
			ret = -1;
			break;
		}

		/* get a destination page */
		dst_page = bvec->bv_page;

		/* get a mapped phyiscal page */
		lblkaddr = dst_page->index;
		pblkaddr = risa_get_mapped_pblkaddr (sbi, lblkaddr);
		if (pblkaddr == NULL_ADDR) {
			ret = -1;
			goto out;
		}

		/* read the requested page */
		if (risa_readpage_flash (sbi, src_page, pblkaddr) != 0) {
			risa_dbg_msg ("risa_read_page_flash failed");
			ret = -1;
			goto out;
		}

		/* copy memory data */
		src_page_addr = (uint8_t*)page_address (src_page);
		dst_page_addr = (uint8_t*)page_address (dst_page);
		memcpy (dst_page_addr, src_page_addr, PAGE_CACHE_SIZE);

		/* go to the next page */
		ClearPageUptodate (src_page);
	}

out:
	/* unlock & free the page */
	unlock_page (src_page);
	__free_pages (src_page, 0);

	if (ret == 0) {
		set_bit (BIO_UPTODATE, &bio->bi_flags);
		bio->bi_end_io (bio, 0);
	} else {
		bio->bi_end_io (bio, ret);
	}
}

static uint8_t risa_is_cp_blk (struct f2fs_sb_info* sbi, block_t lblkaddr)
{
	block_t start_addr =
		le32_to_cpu(F2FS_RAW_SUPER(sbi)->cp_blkaddr);

	if (lblkaddr == start_addr)
		return 1;
	else if (lblkaddr == (start_addr + sbi->blocks_per_seg))
		return 1;

	return 0;
}

void risa_submit_bio (struct f2fs_sb_info* sbi, int rw, struct bio * bio, uint8_t sync)
{
	block_t lblkaddr = bio->bi_sector * 512 / 4096;

	if (risa_is_cp_blk (sbi, lblkaddr)) {
		risa_write_mapping_entries (sbi);
#ifdef RISA_PMU
		atomic64_inc (&sbi->pmu.ckp_w);
#endif
	}

	if (is_valid_meta_lblkaddr (sbi, lblkaddr) == 0) {
		if ((rw & WRITE) == 1) {
#ifdef RISA_PMU
			atomic64_add (bio_sectors (bio) / 8, &sbi->pmu.meta_w);
#endif
			risa_submit_bio_w (sbi, bio, sync);
		} else if (rw == READ || rw == READ_SYNC || rw == READA) {
#ifdef RISA_PMU
			atomic64_add (bio_sectors (bio) / 8, &sbi->pmu.meta_r);
#endif
			risa_submit_bio_r (sbi, bio);
		} else {
			risa_dbg_msg ("[WARNING] unknown type: %d", rw);
			submit_bio (rw, bio);
		}
	} else {
#ifdef RISA_PMU
		if ((rw & WRITE) == 1) {
			atomic64_add (bio_sectors (bio) / 8, &sbi->pmu.norm_w);
		} else {
			atomic64_add (bio_sectors (bio) / 8, &sbi->pmu.norm_r);
		}
#endif
		submit_bio (rw, bio);
	}
}
