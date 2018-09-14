/*
 *	fs/f2fs/risa_ext_dram.h
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


#ifdef RISA_DRAM_META_LOGGING
static int8_t create_dram_metalog (
	struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	uint32_t metalog_size_in_byte;

	/* get the size of memory space for metalog */
	metalog_size_in_byte = ri->nr_metalog_phys_blks * F2FS_BLKSIZE;

	/* allocate the memory space for metalog */
	if ((ri->metalog_dram_buff = (uint8_t*)vmalloc (metalog_size_in_byte)) == NULL) {
		risa_dbg_msg ("Errors occur while allocating the memory space for metalog_dram_buff");
		return -1;
	}

	/* initialize metalog space */
	memset (ri->metalog_dram_buff, 0x00, metalog_size_in_byte);

	return 0;
}

static int8_t build_dram_metalog (
	struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);
	struct page* page = NULL;
	uint32_t blkofs = 0;
	int8_t ret = 0;

	/* get the free page from the memory pool */
	page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (page)) {
		risa_dbg_msg ("Errors occur while allocating page");
		return PTR_ERR (page);
	}
	lock_page (page);

	/* read the snapshot from the disk */
	for (blkofs = 0; blkofs < ri->nr_metalog_phys_blks; blkofs++) {
		uint8_t* ptr_page_addr = NULL;

		/* read the mapping data from NAND devices */
		if (risa_readpage (sbi, page, ri->metalog_blkaddr + blkofs) != 0) {
			risa_dbg_msg ("Errors occur while reading the mapping data from NAND devices");
			ret = -1;
			goto out;
		}

		/* copy the page data to the mapping table  */
		ptr_page_addr = (uint8_t*)page_address (page);
		memcpy (ri->metalog_dram_buff + (blkofs * F2FS_BLKSIZE), ptr_page_addr, F2FS_BLKSIZE);

		/* goto the next page */
		ClearPageUptodate (page);
	}
	
out:
	/* unlock & free the page */
	unlock_page (page);
	__free_pages (page, 0);

	return ret;
}

static void destroy_dram_metalog (
	struct f2fs_sb_info* sbi)
{
	struct risa_info* ri = RISA_RI (sbi);

	if (ri->metalog_dram_buff != NULL) {
		vfree (ri->metalog_dram_buff);
		ri->metalog_dram_buff = NULL;
	}
}

int8_t risa_readpage_dram (
	struct f2fs_sb_info* sbi, 
	struct page* page, 
	block_t pblkaddr)
{
	struct risa_info* ri = RISA_RI (sbi);
	uint8_t* ptr_page_addr = NULL;
	uint32_t metalog_dram_blkofs;
	
	/* is valid physical address? */
	if (is_valid_meta_pblkaddr (sbi, pblkaddr) != 0) {
		risa_dbg_msg ("[ERROR] hmm... invalid physical address (%llu)", (int64_t)pblkaddr);
		return -1;
	}

	/* get the virtual address of the page */
	ptr_page_addr = (uint8_t*) page_address (page);
	if (IS_ERR (page)) {
		risa_dbg_msg ("[ERROR] there are some errors in the page");
		return -1;
	}

	/* copy the data in the dram metalog to the virtual address */
	metalog_dram_blkofs = pblkaddr - ri->metalog_blkaddr;
	memcpy (
		ptr_page_addr, 
		ri->metalog_dram_buff + metalog_dram_blkofs * F2FS_BLKSIZE, 
		F2FS_BLKSIZE);

	return 0;
}

int8_t risa_writepage_dram (
	struct f2fs_sb_info* sbi, 
	struct page* page, 
	block_t pblkaddr)
{
	struct risa_info* ri = RISA_RI (sbi);
	uint8_t* ptr_page_addr = NULL;
	uint32_t metalog_dram_blkofs;

	/* is valid physical address? */
	if (is_valid_meta_pblkaddr (sbi, pblkaddr) != 0) {
		risa_dbg_msg ("[ERROR] hmm... invalid physical address (%llu)", (int64_t)pblkaddr);
		return -1;
	}
	
	/* get the virtual address of the page */
	ptr_page_addr = (uint8_t*) page_address (page);
	if (IS_ERR (page)) {
		risa_dbg_msg ("[ERROR] there are some errors in the page");
		return -1;
	}

	/* copy the data in the dram metalog to the virtual address */
	metalog_dram_blkofs = pblkaddr - ri->metalog_blkaddr;
	memcpy (
		ri->metalog_dram_buff + metalog_dram_blkofs * F2FS_BLKSIZE, 
		ptr_page_addr, 
		F2FS_BLKSIZE);

	return 0;
}
#endif

