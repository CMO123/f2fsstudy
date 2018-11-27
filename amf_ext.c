

#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>


#include "f2fs.h"
#include "amf_ext.h"
#include "segment.h"
#include "trace.h"
#include <trace/events/f2fs.h>


#include "tgt.h"

static void destroy_metalog_summary_table (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	if (ri->summary_table) {
		kfree (ri->summary_table);
		ri->summary_table = NULL;
	}
}


static void destroy_metalog_mapping_table (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	if (ri->map_blks) {
		kfree (ri->map_blks);
		ri->map_blks = NULL;
	}
}


int32_t is_valid_meta_pblkaddr (struct f2fs_sb_info* sbi, block_t pblkaddr)
{
	struct amf_info* ri = AMF_RI (sbi);

	if (sbi->ri == NULL)
		return -1;
	
	if (pblkaddr >= ri->metalog_blkofs &&
		pblkaddr < ri->metalog_blkofs + ri->nr_metalog_phys_blks)
		return 0;

	return -1;
}

int8_t amf_readpage_dram (	struct f2fs_sb_info* sbi, 	struct page* page, 	block_t pblkaddr)
{
	struct amf_info* ri = AMF_RI (sbi);
	uint8_t* ptr_page_addr = NULL;
	uint32_t metalog_dram_blkofs;
	
	/* is valid physical address? */
	if (is_valid_meta_pblkaddr (sbi, pblkaddr) != 0) {
		amf_dbg_msg ("[ERROR] hmm... invalid physical address (%llu)", (int64_t)pblkaddr);
		return -1;
	}

	/* get the virtual address of the page */
	ptr_page_addr = (uint8_t*) page_address (page);
	if (IS_ERR (page)) {
		amf_dbg_msg ("[ERROR] there are some errors in the page");
		return -1;
	}

	/* copy the data in the dram metalog to the virtual address */
	metalog_dram_blkofs = pblkaddr - ri->metalog_blkofs;
	memcpy (
		ptr_page_addr, 
		ri->metalog_dram_buff + metalog_dram_blkofs * F2FS_BLKSIZE, 
		F2FS_BLKSIZE);

	return 0;
}

int8_t amf_readpage (struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr)
{
	int8_t ret;
#ifdef AMF_DRAM_META_LOGGING
	ret = amf_readpage_dram (sbi, page, pblkaddr);
#else
	/*down_read (&sbi->bio_sem);
	ret = risa_readpage_flash (sbi, page, pblkaddr);
	up_read (&sbi->bio_sem);
	*/
	tgt_submit_page_read_sync(sbi, page, pblkaddr);
	//page = get_meta_page(sbi, pblkaddr);
#endif
	return ret;
}
int8_t amf_writepage_dram (struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr)
{
	struct amf_info* ri = AMF_RI (sbi);
	uint8_t* ptr_page_addr = NULL;
	uint32_t metalog_dram_blkofs;

	/* is valid physical address? */
	if (is_valid_meta_pblkaddr (sbi, pblkaddr) != 0) {
		amf_dbg_msg ("[ERROR] hmm... invalid physical address (%llu)", (int64_t)pblkaddr);
		return -1;
	}
	
	/* get the virtual address of the page */
	ptr_page_addr = (uint8_t*) page_address (page);
	if (IS_ERR (page)) {
		amf_dbg_msg ("[ERROR] there are some errors in the page");
		return -1;
	}

	/* copy the data in the dram metalog to the virtual address */
	metalog_dram_blkofs = pblkaddr - ri->metalog_blkofs;
	memcpy (
		ri->metalog_dram_buff + metalog_dram_blkofs * F2FS_BLKSIZE, 
		ptr_page_addr, 
		F2FS_BLKSIZE);

	return 0;
}

int8_t amf_writepage (struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr, 
							uint8_t sync)
{
	int8_t ret;
#ifdef AMF_DRAM_META_LOGGING
	ret = amf_writepage_dram (sbi, page, pblkaddr); 
#else
	//down_write (&sbi->bio_sem);
	ret = tgt_submit_page_write(sbi, page, pblkaddr, sync);
	//up_write (&sbi->bio_sem);
#endif
	return ret;
}


static int8_t build_dram_metalog (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	struct page* page = NULL;
	uint32_t blkofs = 0;
	int8_t ret = 0;



	/* get the free page from the memory pool */
	page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (page)) {
		amf_dbg_msg ("Errors occur while allocating page");
		return PTR_ERR (page);
	}
	lock_page (page);

	/* read the snapshot from the disk */
	for (blkofs = 0; blkofs < ri->nr_metalog_phys_blks; blkofs++) {
		uint8_t* ptr_page_addr = NULL;

		/* read the mapping data from NAND devices */
		/*if (amf_readpage (sbi, page, ri->metalog_blkofs + blkofs) != 0) {
			amf_dbg_msg ("Errors occur while reading the mapping data from NAND devices");
			ret = -1;
			goto out;
		}*/
		if (tgt_submit_page_read_sync (sbi, page, ri->metalog_blkofs + blkofs) != 0) {
			amf_dbg_msg ("Errors occur while reading the mapping data from NAND devices");
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


static int8_t create_dram_metalog (
	struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	uint32_t metalog_size_in_byte;


	/* get the size of memory space for metalog */
	metalog_size_in_byte = ri->nr_metalog_phys_blks * F2FS_BLKSIZE;

	/* allocate the memory space for metalog */
	if ((ri->metalog_dram_buff = (uint8_t*)vmalloc (metalog_size_in_byte)) == NULL) {
		amf_dbg_msg ("Errors occur while allocating the memory space for metalog_dram_buff");
		return -1;
	}

	/* initialize metalog space */
	memset (ri->metalog_dram_buff, 0x00, metalog_size_in_byte);

	return 0;
}

static int32_t create_metalog_summary_table (struct f2fs_sb_info* sbi)
{//创建meta-log的summary table
	struct amf_info* ri = AMF_RI (sbi);
	uint32_t sum_length = 0;
	uint32_t i = 0, j = 0;
	uint8_t is_dead = 1;
	int32_t ret = 0;
//pr_notice("Enter create_metalog_summary_table()\n");
	/* get the geometry information */
	sum_length = (sizeof (uint8_t) * ri->nr_metalog_phys_blks + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE;//需要几个block？

	amf_msg ("--------------------------------");
	amf_msg (" * summary table length: %u", sum_length);
	amf_msg ("--------------------------------");

	/* allocate the memory space for the summary table */
	if ((ri->summary_table = 
			(uint8_t*)kmalloc (sum_length * F2FS_BLKSIZE, GFP_KERNEL)) == NULL) {
		amf_dbg_msg ("Errors occur while allocating memory space for the mapping table");
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
				amf_msg ("summary: set phyofs %u to valid", le32_to_cpu (phyofs) - ri->metalog_blkofs);
				ri->summary_table[le32_to_cpu (phyofs) - ri->metalog_blkofs] = 1;
				
			}
		}
	}

	/* search for a section that contains only invalid blks,只找一个即可 */
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

			//risa_do_trim (sbi, ri->mapping_blkofs + ri->metalog_gc_eblkofs, ri->blks_per_sec); 
			ret = tgt_submit_addr_erase_async(sbi, ri->mapping_blkofs + ri->metalog_gc_eblkofs, ri->blks_per_sec);
			//mdelay(10000);
			//f2fs_issue_discard(sbi, ri->mapping_blkofs + ri->metalog_gc_eblkofs, ri->blks_per_sec);
			memset (&ri->summary_table[i*ri->blks_per_sec], 0x00, ri->blks_per_sec);
			break;
		}
	}

	/* metalog must have at least one dead section */
	if (is_dead == 0) {
		amf_msg ("[ERROR] oops! cannot find dead sections in metalog");
		ret = -1;
	} else {
		amf_msg ("-------------------------------");
		amf_msg ("ri->metalog_gc_slbkofs: %u (%u)", 
			ri->metalog_gc_sblkofs, ri->metalog_blkofs + ri->metalog_gc_sblkofs);
		amf_msg ("ri->metalog_gc_eblkofs: %u (%u)", 
			ri->metalog_gc_eblkofs, ri->metalog_blkofs + ri->metalog_gc_eblkofs);
		amf_msg ("-------------------------------");
	}

	//pr_notice("End create_metalog_summary()\n");
	//mdelay(20000);
out:
	return ret;
}


/*
 * Create mapping & summary tables 
 */
static int32_t create_metalog_mapping_table (struct f2fs_sb_info* sbi)
{//扫描mapping的所有blk
	struct amf_info* ri = AMF_RI (sbi);
	struct page* page;

	uint32_t i = 0, j = 0;
	uint8_t is_dead_section = 1;
	int32_t ret = 0;
//pr_notice("Enter create_metalog_mapping_table()\n");
	/* get the geometry information */
	ri->nr_mapping_phys_blks = NR_MAPPING_SECS * ri->blks_per_sec;
	ri->nr_mapping_logi_blks = ri->nr_metalog_logi_blks / 1020;//metalog逻辑blk数目/1020，即metalog需要多少个blk来映射metalog数据地址
	//这里1020是因为，amf_map_blk中之能存储F2FS_BLKSIZE/sizeof(__le32)-4 = 1020个映射项，所以这里得到的是amf_map_blk个数目
	if (ri->nr_metalog_logi_blks % 1020 != 0) {
		ri->nr_mapping_logi_blks++;
	}
	//映射表总共有ri->nr_metalog_logi_blks个项，

	amf_msg ("--------------------------------");
	amf_msg (" # of mapping entries: %u", ri->nr_metalog_logi_blks);
	amf_msg (" * mapping table blkaddr: %u (blk)", ri->mapping_blkofs);
	amf_msg (" * mapping table length: %u (blk)", ri->nr_mapping_phys_blks);

	//mdelay(5000);
	/* allocate the memory space for the summary table */
	if ((ri->map_blks = (struct amf_map_blk*)kmalloc (
			sizeof (struct amf_map_blk) * ri->nr_mapping_logi_blks, GFP_KERNEL)) == NULL) {
		amf_dbg_msg ("Errors occur while allocating memory space for the mapping table");
		mdelay(10000);
		goto out;
	}
	memset (ri->map_blks, 0x00, sizeof (struct amf_map_blk) * ri->nr_mapping_logi_blks);
	//pr_notice("create ri->map_blks\n");
	//mdelay(5000);		
	/* get the free page from the memory pool */
	page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (page)) {
		amf_dbg_msg ("Errors occur while allocating page");
		mdelay(10000);
		kfree (ri->map_blks);
		ret = PTR_ERR (page);
		goto out;
	}
	lock_page (page);

	//pr_notice("alloc_page\n");
	//mdelay(5000);
	
	/* read the mapping info from the disk */
	ri->mapping_gc_sblkofs = -1;
	ri->mapping_gc_eblkofs = -1;

	/* read the mapping info from the disk */
	for (i = 0; i < NR_MAPPING_SECS; i++) {//扫描一遍mapping
		is_dead_section = 1;
		for (j = 0; j < ri->blks_per_sec; j++) {
			__le32* ptr_page_addr = NULL;
			struct amf_map_blk* new_map_blk = NULL;

			/* read the mapping data from NAND devices */
			/*printk (KERN_INFO "MR: %lu\n", ri->mapping_blkofs + (i * ri->blks_per_sec) + j);*/
			/*down_read (&sbi->bio_sem);
			if (amf_readpage_flash (sbi, page, ri->mapping_blkofs + (i * ri->blks_per_sec) + j) != 0) {//仅读取，没有放入page cache中
				up_read (&sbi->bio_sem);
				amf_dbg_msg ("Errors occur while reading the mapping data from NAND devices");
				ret = -1;
				goto out;
			}
			up_read (&sbi->bio_sem);
			*/
			//page = get_meta_page(sbi, ri->mapping_blkofs + (i * ri->blks_per_sec) + j));
			
			
			/* get the virtual address from the page */
			ptr_page_addr = (__le32*)page_address (page);
			new_map_blk = (struct amf_map_blk*)ptr_page_addr;
			memset (ptr_page_addr, 0, F2FS_BLKSIZE);

			//pr_notice("tgt_submit_page_read(sbi, page, ri->mapping_blkofs + (i * ri->blks_per_sec) + j = %d)\n",ri->mapping_blkofs + (i * ri->blks_per_sec) + j);//512
			int index = ri->mapping_blkofs + (i * ri->blks_per_sec) + j;
			ret = tgt_submit_page_read_sync(sbi, page, index);
			//pr_notice("ret = %d\n",ret);
			if(ret != 0){
				amf_dbg_msg("create_metalog_mapping_table(): error in tgt_submit_page_read.\n");
				pr_notice("new_map_blk->magic = %u,new_map_blk->index = %u, new_map_blk->mapping[0]=%u\n",new_map_blk->magic, new_map_blk->index, new_map_blk->mapping[0]);
			}
			
			/* check version # */
			if (new_map_blk->magic == cpu_to_le32 (0xEF)) {
				uint32_t index = le32_to_cpu (new_map_blk->index);
				//amf_msg ("index: %u (old ver: %u, new ver: %u)", index, le32_to_cpu (ri->map_blks[index/1020].ver), le32_to_cpu (new_map_blk->ver));
				if (le32_to_cpu (ri->map_blks[index/1020].ver) <= le32_to_cpu (new_map_blk->ver)) {
					/*risa_msg ("copy new map blk");*/
					memcpy (&ri->map_blks[index/1020], ptr_page_addr, F2FS_BLKSIZE);//为什么这里要除以1020？？？？应该是每个map_blks只记录cp、nat、sit、ssa等
					is_dead_section = 0; /* this section has a valid blk */
				}
			}
		
			/* goto the next page */
			ClearPageUptodate (page);
		}
	//	pr_notice("End submit()\n");
	//	mdelay(5000);
		/* is it dead? */
		if (is_dead_section == 1) {//如果第i个section是dead的
			printk (KERN_INFO "dead section detected: %u\n", i);
			if (ri->mapping_gc_eblkofs == -1 && ri->mapping_gc_sblkofs == -1) {
				ri->mapping_gc_eblkofs = i * ri->blks_per_sec;
				ri->mapping_gc_sblkofs = i * ri->blks_per_sec + ri->blks_per_sec;//因为前面还有一个sec0？？这样回收0则刚好是sec1？
				ri->mapping_gc_sblkofs = ri->mapping_gc_sblkofs % ri->nr_mapping_phys_blks;
				//amf_do_trim (sbi, ri->mapping_blkofs + ri->mapping_gc_eblkofs, ri->blks_per_sec); 
				//f2fs_issue_discard(sbi, ri->mapping_blkofs + ri->mapping_gc_eblkofs, ri->blks_per_sec);
				tgt_mapping_erase(sbi, ri->mapping_blkofs + ri->mapping_gc_eblkofs, ri->blks_per_sec);
				//mdelay(10000);
				
			}
		}
		//mdelay(10000);
	}
	//pr_notice("End is_dead_section()\n");
	//		mdelay(5000);

	/* is there a free section for the mapping table? */
	if (ri->mapping_gc_sblkofs == -1 || ri->mapping_gc_eblkofs == -1) {
		amf_msg ("[ERROR] oops! there is no free space for the mapping table");
		ret = -1;
	} else {
		amf_msg ("-------------------------------");
		amf_msg ("ri->mapping_gc_slbkofs: %u (%u)", 
			ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
		amf_msg ("ri->mapping_gc_eblkofs: %u (%u)", 
			ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
		amf_msg ("-------------------------------");
	}

	/*pr_notice("ri->map_blks[0].index = %d, ri->map_blks[0].magic= %d,ri->map_blks[0].mapping[0] = %d,ri->map_blks[0].mapping[1] = %d, \
		ri->map_blks[0].mapping[2] = %d,ri->map_blks[0].mapping[27] = %d,ri->map_blks[0].mapping[28] = %d,ri->map_blks[1].index = %d,ri->map_blks[1].mapping[0] = %d\n",ri->map_blks[0].index, ri->map_blks[0].magic,ri->map_blks[0].mapping[0],ri->map_blks[0].mapping[1],
		ri->map_blks[0].mapping[2],ri->map_blks[0].mapping[27],ri->map_blks[0].mapping[28],ri->map_blks[1].index,ri->map_blks[1].mapping[0]);
	*/

out:
	//	pr_notice("End out\n");
	//	mdelay(5000);

	/* unlock & free the page */
	unlock_page (page);
	__free_pages (page, 0);
	
	return ret;
}


/* 
 * create the structure for RISA (ri) 
 */
int32_t amf_create_ri (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = NULL;
	uint32_t nr_logi_metalog_segments = 0;
	uint32_t nr_phys_metalog_segments = 0;

	/* create amf_info structure */
	if ((ri = (struct amf_info*)kmalloc (
			sizeof (struct amf_info), GFP_KERNEL)) == NULL) {
		amf_dbg_msg ("Errors occur while creating amf_info");
		return -1;
	}
	sbi->ri = ri;


	

	/* initialize some variables */
	ri->mapping_blkofs = get_mapping_blkofs (sbi);//sec1的起始地址
	ri->metalog_blkofs = get_metalog_blkofs (sbi);//sec4的起始地址

	nr_logi_metalog_segments = get_nr_logi_meta_segments (sbi);//cp，sit，nat，ssa个数 = 56
	nr_phys_metalog_segments = get_nr_phys_meta_segments (sbi, nr_logi_metalog_segments);

	ri->nr_metalog_logi_blks = SEGS2BLKS (sbi, nr_logi_metalog_segments);
	ri->nr_metalog_phys_blks = SEGS2BLKS (sbi, nr_phys_metalog_segments);

	ri->blks_per_sec = sbi->segs_per_sec * (1 << sbi->log_blocks_per_seg);

	
	/* create mutex for GC */
	mutex_init(&ri->amf_gc_mutex);

	/* display information about metalog */
	amf_msg ("--------------------------------");
	amf_msg (" * mapping_blkofs: %u", ri->mapping_blkofs);
	amf_msg (" * metalog_blkofs: %u", ri->metalog_blkofs);
	amf_msg (" * # of blks per sec: %u", ri->blks_per_sec);
	amf_msg (" * # of logical meta-log blks: %u", ri->nr_metalog_logi_blks);
	amf_msg (" * # of physical meta-log blks: %u", ri->nr_metalog_phys_blks);
	amf_msg (" * the range of logical meta address: %u - %u", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_logi_blks);
	amf_msg (" * the range of physical meta address: %u - %u", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_phys_blks);


	return 0;
}



int32_t amf_build_ri (struct f2fs_sb_info *sbi)
{
//pr_notice("Enter amf_build_ri()\n");
	/* see if ri is initialized or not */
	if (sbi == NULL || sbi->ri == NULL) {
		amf_dbg_msg ("Error occur because some input parameters are NULL");
		return -1;
	}

	/* build meta-log mapping table */
	if (create_metalog_mapping_table (sbi) != 0) {
		amf_dbg_msg ("Errors occur while creating the metalog mapping table");
		goto error_metalog_mapping;
	}
	
	/* build meta-log summary table */
	if (create_metalog_summary_table (sbi) != 0) {
		amf_dbg_msg ("Errors occur while creating the metalog summary table");
		goto error_metalog_summary;
	}
	//mdelay(20000);
#ifdef AMF_DRAM_META_LOGGING
	if (create_dram_metalog (sbi) != 0) {
		amf_dbg_msg ("Errors occur while creating the dram metalog");
		goto error_create_dram_metalog;
	}
	
	if (build_dram_metalog (sbi) != 0) {
		amf_dbg_msg ("Errors occur while building the dram metalog");
		goto error_build_dram_metalog;
	}
	
#endif


	return 0;

#ifdef AMF_DRAM_META_LOGGING
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



/*
 * mapping table management 
 */
int32_t get_mapping_free_blks (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	uint32_t nr_free_blks;

	if (ri->mapping_gc_sblkofs < ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->nr_mapping_phys_blks - ri->mapping_gc_eblkofs + ri->mapping_gc_sblkofs;
	} else if (ri->mapping_gc_sblkofs > ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->mapping_gc_sblkofs - ri->mapping_gc_eblkofs;
	} else {
		amf_dbg_msg ("[ERROR] 'ri->mapping_gc_sblkofs (%u)' is equal to 'ri->mapping_gc_eblkofs (%u)'", 
			ri->mapping_gc_sblkofs, ri->mapping_gc_eblkofs);
		nr_free_blks = -1;
	}
	
//pr_notice("get_mapping_free_blks() return nr_free_blks = %d\n", nr_free_blks);
	return nr_free_blks;
}

int8_t is_mapping_gc_needed (struct f2fs_sb_info* sbi, int32_t nr_free_blks)
{
	if (nr_free_blks <= (sbi->segs_per_sec * sbi->blocks_per_seg)) {
		return 0;
	}
	return -1;
}

int8_t amf_do_mapping_gc (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	int ret;
	
	/*
	amf_dbg_msg ("before gc");
	amf_msg ("-------------------------------");
	amf_msg ("ri->mapping_gc_slbkofs: %u (%u)", 
		ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
	amf_msg ("ri->mapping_gc_eblkofs: %u (%u)", 
		ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
	amf_msg ("-------------------------------");
	*/

	/* perform gc */
	//risa_do_trim (sbi, ri->mapping_blkofs + ri->mapping_gc_sblkofs, ri->blks_per_sec); 
	//f2fs_issue_discard(sbi,  ri->mapping_blkofs + ri->mapping_gc_sblkofs,ri->blks_per_sec);
	ret = tgt_mapping_erase(sbi,  ri->mapping_blkofs + ri->mapping_gc_sblkofs,ri->blks_per_sec);
	/* advance 'mapping_gc_sblkofs' */
	ri->mapping_gc_sblkofs = (ri->mapping_gc_sblkofs + ri->blks_per_sec) % 
		ri->nr_mapping_phys_blks;

	/*
	amf_dbg_msg ("after gc");
	amf_msg ("-------------------------------");
	amf_msg ("ri->mapping_gc_slbkofs: %u (%u)", 
		ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
	amf_msg ("ri->mapping_gc_eblkofs: %u (%u)", 
		ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
	amf_msg ("-------------------------------");
	*/

	return 0;
}


int32_t amf_write_mapping_entries (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	struct page* page = NULL;
	int32_t nr_free_blks = 0;
	uint32_t i = 0;

	/* see if gc is needed for the mapping area */
	nr_free_blks = get_mapping_free_blks (sbi);//512
	if (is_mapping_gc_needed (sbi, nr_free_blks) == 0) {
		amf_do_mapping_gc (sbi);
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
			amf_dbg_msg ("Errors occur while allocating a new page");
			return PTR_ERR (page);
		}
		lock_page (page);

		/* write dirty entires to NAND flash */
		ptr_page_addr = (__le32*)page_address (page);
		memcpy (ptr_page_addr, &ri->map_blks[i], F2FS_BLKSIZE);
		/*risa_dbg_msg ("writing mapping (%u): %u", */
		/*ri->map_blks[i].index, ri->mapping_blkofs + ri->mapping_gc_eblkofs);*/
		/*risa_writepage_flash (sbi, page, ri->mapping_blkofs + ri->mapping_gc_eblkofs, 1);*/
		//risa_writepage_flash (sbi, page, ri->mapping_blkofs + ri->mapping_gc_eblkofs, 0);
		//write_meta_page(sbi, page,FS_META_IO);
		tgt_submit_page_write_sync(sbi, page, ri->mapping_blkofs + ri->mapping_gc_eblkofs);

		/* update physical location */
		ri->mapping_gc_eblkofs = 
			(ri->mapping_gc_eblkofs + 1) % ri->nr_mapping_phys_blks;

		atomic64_add (1, &sbi->pmu.mapping_w);
	}


	return 0;
}

static void destroy_dram_metalog (
	struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);

	if (ri->metalog_dram_buff != NULL) {
		vfree (ri->metalog_dram_buff);
		ri->metalog_dram_buff = NULL;
	}
}

	static void destroy_ri (struct f2fs_sb_info* sbi)
	{
		if (sbi->ri) {
			kfree (sbi->ri);
			sbi->ri = NULL;
		}
	}

void amf_destory_ri (struct f2fs_sb_info* sbi)
{
#ifdef AMF_DRAM_META_LOGGING
	destroy_dram_metalog (sbi);
#endif
	destroy_metalog_summary_table (sbi);
	destroy_metalog_mapping_table (sbi);
	destroy_ri (sbi);
}

//============================================================================================
static uint8_t amf_is_cp_blk (struct f2fs_sb_info* sbi, block_t lblkaddr)
{
	block_t start_addr =
		le32_to_cpu(F2FS_RAW_SUPER(sbi)->cp_blkaddr);

	if (lblkaddr == start_addr)
		return 1;
	else if (lblkaddr == (start_addr + sbi->blocks_per_seg))
		return 1;

	return 0;
}


/*
 * metalog management 
 */
int32_t is_valid_meta_lblkaddr (struct f2fs_sb_info* sbi, 
	block_t lblkaddr)
{
	struct amf_info* ri = AMF_RI (sbi);

	if (sbi->ri == NULL)
		return -1;
	
	if (lblkaddr >= ri->metalog_blkofs &&
		lblkaddr < ri->metalog_blkofs + ri->nr_metalog_logi_blks)
		return 0;

	return -1;
}

	
uint32_t amf_get_new_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr, uint32_t length)
{
	struct amf_info* ri = AMF_RI (sbi);
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
			amf_dbg_msg ("pblkaddr is invalid (%u)", pblkaddr);
			return NULL_ADDR;
		}
	} else {
		amf_dbg_msg ("metalog_gc_eblkofs is NOT free: summary_table[%u] = %u",
			ri->metalog_gc_eblkofs, ri->summary_table[ri->metalog_gc_eblkofs]);
		return NULL_ADDR;
	}

	return pblkaddr;
}


int8_t amf_map_l2p (struct f2fs_sb_info* sbi, block_t lblkaddr, block_t pblkaddr, uint32_t length)
{
	struct amf_info* ri = AMF_RI (sbi);
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
			amf_dbg_msg ("is_valid_meta_lblkaddr is failed (cur_lblkaddr: %u)", cur_lblkaddr);
			return -1;
		}

		/* get the new pblkaddr */
		if (cur_pblkaddr == NULL_ADDR) {
			if ((cur_pblkaddr = amf_get_new_pblkaddr (sbi, cur_lblkaddr, length)) == NULL_ADDR) {
				amf_dbg_msg ("cannot get the new free block (cur_lblkaddr: %u)", cur_lblkaddr);
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
			/*if (amf_do_trim (sbi, prev_pblkaddr, 1) == -1) {
				amf_dbg_msg (KERN_INFO "Errors occur while trimming the page during risa_map_l2p");
			}*/

			/*
			if (tgt_submit_addr_erase_async(sbi, prev_pblkaddr, 1) == -1) {
				amf_dbg_msg (KERN_INFO "Errors occur while trimming the page during risa_map_l2p");
			}
			*/
		} else if (prev_pblkaddr != NULL_ADDR) {
			amf_dbg_msg ("invalid prev_pblkaddr = %llu", (int64_t)prev_pblkaddr);
		} else {
			/* it is porible that 'prev_pblkaddr' is invalid */
		}

		/* update the mapping & summary table */
		new_lblkaddr = cur_lblkaddr - ri->metalog_blkofs;
		ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020] = cpu_to_le32 (cur_pblkaddr);
		ri->map_blks[new_lblkaddr/1020].dirty = 1;

		ri->summary_table[cur_pblkaddr - ri->metalog_blkofs] = 1; /* set to valid */

		/* adjust end_blkofs in the meta-log */
		ri->metalog_gc_eblkofs = (ri->metalog_gc_eblkofs + 1) % (ri->nr_metalog_phys_blks);//分配了一个新的paddr

		/* go to the next logical blkaddr */
		cur_lblkaddr++;
		cur_pblkaddr = NULL_ADDR;
	}

	return 0;
}

//=================================读写==============================================================================================
static inline sector_t amf_get_lba(struct bio *bio)
{
	return bio->bi_iter.bi_sector >> F2FS_LOG_SECTORS_PER_BLOCK;
}
static inline unsigned int amf_get_secs(struct bio *bio)
{
	return  bio->bi_iter.bi_size / F2FS_BLKSIZE;
}
/*这两个是main area的数据读写*/

void amf_submit_bio_read_async (struct f2fs_sb_info* sbi, struct bio* bio)
{
	struct nvm_tgt_dev *dev = sbi->s_lightpblk->tgt_dev;
	struct block_device* bdev = sbi->sb->s_bdev;
	struct nvm_geo* geo = &dev->geo;
	struct nvm_rq *rqd;
	uint32_t lblkaddr = amf_get_lba(bio);
	unsigned int nr_secs = amf_get_secs(bio); 
	int i, j =0;
	int ret = NVM_IO_ERR;

//pr_notice("amf_submit_bio_read()\n");
	/*设置rqd*/
	rqd = kzalloc(sizeof(struct nvm_rq), GFP_KERNEL);
	rqd->dev = dev;
	rqd->opcode = NVM_OP_PREAD;
	 
	rqd->bio = bio;
	rqd->nr_ppas = nr_secs;
	rqd->private = sbi;
	rqd->end_io = tgt_end_io_read;
	rqd->meta_list = nvm_dev_dma_alloc(dev->parent, GFP_KERNEL,
							&rqd->dma_meta_list);	
	if (!rqd->meta_list) {
			pr_err("pblk: not able to allocate ppa list\n");
			goto fail_rqd_free;
	}
	if (nr_secs > 1) {
		rqd->ppa_list = rqd->meta_list + tgt_dma_meta_size;
		rqd->dma_ppa_list = rqd->dma_meta_list + tgt_dma_meta_size;
		
		rqd->flags = NVM_IO_SUSPEND | NVM_IO_SCRAMBLE_ENABLE;	
		rqd->flags |= geo->plane_mode >> 1;//sequential读,对齐以后顺序读？？
		//to do list，上下sector大小不对齐问题

		for(i = 0; i < nr_secs; i++){
			rqd->ppa_list[j++] = addr_ppa32_to_ppa64(sbi, lblkaddr+i);
			//pr_notice("rqd->ppa_list = 0x%llx\n", rqd->ppa_list[j-1]);
		}		
		
	}else{
		rqd->ppa_addr = addr_ppa32_to_ppa64(sbi, lblkaddr);
		//pr_notice("rqd->ppa_addr = 0x%llx\n", rqd->ppa_addr);
		rqd->flags = NVM_IO_SUSPEND | NVM_IO_SCRAMBLE_ENABLE;	
	}

	ret =  nvm_submit_io(dev, rqd);

	if(rqd->error){
		pr_notice("amf: read error:%d\n", rqd->error);
	}
	if(ret == NVM_IO_DONE){
		bio_endio(bio);
		return NVM_IO_OK;
	}
	if(ret == NVM_IO_ERR){
		pr_err("amf: read IO submission failed\n"); 
		bio_io_error(bio);
		return ret;
	}
	
	return NVM_IO_OK;	

fail_rqd_free:
fail_end_io:
	nvm_dev_dma_free(dev->parent, rqd->meta_list, rqd->dma_meta_list);
	kfree(rqd);
	return ret;
}
/*这两个是main area的数据读写*/
static int count = 0;
void amf_submit_bio_read_sync (struct f2fs_sb_info* sbi, struct bio* bio)
{
	struct nvm_tgt_dev *dev = sbi->s_lightpblk->tgt_dev;
	struct block_device* bdev = sbi->sb->s_bdev;
	struct nvm_geo* geo = &dev->geo;
	struct nvm_rq rqd;
	uint32_t lblkaddr = amf_get_lba(bio);
	unsigned int nr_secs = amf_get_secs(bio); 
	int i, j =0;
	int ret = NVM_IO_ERR;

	//pr_notice("amf_submit_bio_read()\n");
	/*设置rqd*/
		
	rqd.bio = bio;
	
	tgt_setup_r_rq(sbi, &rqd, nr_secs, tgt_end_io_read);
	if (nr_secs > 1) {
		for(i = 0; i < nr_secs; i++){
			rqd.ppa_list[j++] = addr_ppa32_to_ppa64(sbi, lblkaddr+i);
			//pr_notice("rqd->ppa_list = 0x%llx\n", rqd->ppa_list[j-1]);
		}		
		
	}else{
		rqd.ppa_addr = addr_ppa32_to_ppa64(sbi, lblkaddr);
		//pr_notice("rqd.ppa_addr = 0x%llx\n", rqd.ppa_addr);
	}

	ret = nvm_submit_io_sync(dev, &rqd);
	/*pr_notice("rqd.bio.bi_status = %d\n",rqd.bio->bi_status);
	pr_notice("count = %d\n",++count);
	pr_notice("ret = %d\n",ret);
	pr_notice("rqd.error = %d\n", rqd.error);
	*/
	if(ret){
		pr_err("amf: emeta I/O submission failed: %d\n", ret);
		bio_put(bio);
		goto free_rqd_dma;			
	}
	
free_rqd_dma:
	nvm_dev_dma_free(dev->parent, rqd.meta_list, rqd.dma_meta_list);
	return ret;
}
void amf_submit_bio_write_sync(struct f2fs_sb_info* sbi, struct bio* bio)
{
	struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
	unsigned int nr_secs = amf_get_secs(bio);
	uint32_t lblkaddr = amf_get_lba(bio);
	int i,j=0;
	struct nvm_rq rqd;
	int ret;

//pr_notice("Enter amf_submit_bio_write_sync(), lblkaddr = %d, nr_secs = %d\n", lblkaddr, nr_secs);
retry:	
	/*创建rqd*/
	rqd.bio = bio;
	ret = tgt_setup_w_rq(sbi,&rqd, nr_secs, NULL);
	
	//rqd->ppa_list[0] = addr_to_gen_ppa(sbi, pblkaddr);
	if(nr_secs > 1){
		for(i = 0; i < nr_secs; i++){
		rqd.ppa_list[j++] = addr_ppa32_to_ppa64(sbi, lblkaddr+i);
		//pr_notice("lblkaddr = %d ==> rqd->ppa_list[j] = %d \n", lblkaddr+i, rqd.ppa_list[j-1]);
		}
	}else{
		rqd.ppa_addr = addr_ppa32_to_ppa64(sbi, lblkaddr);
		//pr_notice("lblkaddr = %d ==> rqd->ppa_addr = %d \n", lblkaddr, rqd.ppa_addr);
	}	
	//pr_notice("before nvm_submit_io_sync()\n");

	ret = nvm_submit_io_sync(dev, &rqd);
	if(ret){
		pr_err("amf: amf_submit_bio_write failed:%d\n",ret);
		bio_put(bio);
		goto free_ppa_list;
	}

//pr_notice("End amf_submit_bio_write_sync(), ret = %d\n",ret);
free_ppa_list:
	nvm_dev_dma_free(dev->parent, rqd.meta_list, rqd.dma_meta_list);
	return ret;
	
}

void amf_submit_bio_write(struct f2fs_sb_info* sbi, struct bio* bio)
{
	struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
	unsigned int nr_secs = amf_get_secs(bio);
	uint32_t lblkaddr = amf_get_lba(bio);
	int i,j=0;
	struct nvm_rq* rqd;
	int ret;

//pr_notice("Enter amf_submit_bio_write(), lblkaddr = %d, nr_secs = %d\n", lblkaddr, nr_secs);
retry:	
	/*创建rqd*/
	rqd = kzalloc(sizeof(struct nvm_rq),GFP_KERNEL);
	if (!rqd) {
		cond_resched();
		goto retry;
	}
	//bio->bi_status = 0;
	rqd->bio = bio;
	ret = tgt_setup_w_rq(sbi,rqd, nr_secs, tgt_end_io_write);
	
	//rqd->ppa_list[0] = addr_to_gen_ppa(sbi, pblkaddr);
	if(nr_secs > 1){
		for(i = 0; i < nr_secs; i++){
		rqd->ppa_list[j++] = addr_ppa32_to_ppa64(sbi, lblkaddr+i);
		//pr_notice("lblkaddr = %d ==> rqd->ppa_list[j] = %d \n", lblkaddr+i, rqd->ppa_list[j-1]);
		}
	}else{
		rqd->ppa_addr = addr_ppa32_to_ppa64(sbi, lblkaddr);
		//pr_notice("lblkaddr = %d ==> rqd->ppa_addr = %d \n", lblkaddr, rqd->ppa_addr);
	}	
	//pr_notice("before nvm_submit_io()\n");

	ret = nvm_submit_io(dev, rqd);
	if(ret){
		pr_err("amf: amf_submit_bio_write failed:%d\n",ret);
	}

//pr_notice("End amf_submit_bio_write(), ret = %d\n",ret);
	
}

/*
 由于meta数据需要经过transmap表转换，所以submit bio另外看
*/
void amf_submit_bio_meta_w (struct f2fs_sb_info* sbi, struct bio* bio)
{
		struct page* src_page = NULL;
		struct page* dst_page = NULL;
		struct bio_vec bvec;
		bool sync = op_is_sync(bio_op(bio));
//pr_notice("Enter amf_submit_bio_meta_w(), lblk = %d, nr_sec = %d\n", amf_get_lba(bio),amf_get_secs(bio));
		uint8_t* src_page_addr = NULL;
		uint8_t* dst_page_addr = NULL;
		struct bvec_iter bioloop;
		int8_t ret = 0;
	
		bio_for_each_segment (bvec, bio, bioloop) {
			uint32_t pblkaddr = NULL_ADDR;
			uint32_t lblkaddr = NULL_ADDR;
	
			/* allocate a new page (This page is released later by 'risa_end_io_flash' */
			dst_page = alloc_page (GFP_NOFS | __GFP_ZERO);
			if (IS_ERR (dst_page)) {
				amf_dbg_msg ("Errors occur while allocating page");
				bio->bi_end_io (bio);
				return;
			}
			lock_page (dst_page);
	
			/* check error cases */
			if (bvec.bv_len == 0 || bvec.bv_page == NULL)  {
				amf_dbg_msg ("bvec is wrong");
				break;
			}
	
			/* get the soruce page */
			src_page = bvec.bv_page;
	
			/* get the new pblkaddr */
			spin_lock (&sbi->mapping_lock);
			lblkaddr = src_page->index;
			pblkaddr = amf_get_new_pblkaddr (sbi, lblkaddr, 1);
			if (pblkaddr == NULL_ADDR) {
				spin_unlock (&sbi->mapping_lock);
				amf_dbg_msg ("amf_get_new_pblkaddr failed");
				ret = -1;
				goto out;
			}
			/* update mapping table */
			if (amf_map_l2p (sbi, lblkaddr, pblkaddr, 1) != 0) {
				spin_unlock (&sbi->mapping_lock);
				amf_dbg_msg ("amf_map_l2p failed");
				ret = -1;
				goto out;
			}
			spin_unlock (&sbi->mapping_lock);
	
			/* write the requested page */
			src_page_addr = (uint8_t*)page_address (src_page);
			dst_page_addr = (uint8_t*)page_address (dst_page);
			memcpy (dst_page_addr, src_page_addr, PAGE_SIZE);

//			pr_notice("before submit_page_write(), lblkaddr = %d ==> pblkaddr = %d \n", lblkaddr, pblkaddr);
			//mdelay(6000);
	
			if (tgt_submit_page_write(sbi, dst_page, pblkaddr, 1) != 0) {
				amf_dbg_msg ("amf_write_page_flash failed");
				ret = -1;
				goto out;
			}
		}
//pr_notice("End amf_submit_bio_meta_w() ret = %d\n",ret);
		//mdelay(6000);
	out:
		if (ret == 0) {
			//set_bit (BIO_UPTODATE, &bio->bi_flags);
			bio->bi_status = 0;
			bio->bi_end_io (bio);
		} else {
			bio->bi_end_io (bio);
		}
}

void amf_submit_bio_meta_r (struct f2fs_sb_info* sbi, struct bio* bio)
{
	struct page* src_page = NULL;
	struct page* dst_page = NULL;
	struct bio_vec bvec;

	uint8_t* src_page_addr = NULL;
	uint8_t* dst_page_addr = NULL;
	struct bvec_iter bioloop;
	int8_t ret = 0;
//pr_notice("Enter amf_submit_bio_meta_r()\n");
	src_page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (src_page)) {
		amf_dbg_msg ("Errors occur while allocating page");
		bio->bi_end_io (bio);
		return;
	}
	lock_page (src_page);

	bio_for_each_segment (bvec, bio, bioloop) {
		uint32_t pblkaddr = NULL_ADDR;
		uint32_t lblkaddr = NULL_ADDR;

		/* check error cases */
		if (bvec.bv_len == 0 || bvec.bv_page == NULL)  {
			amf_dbg_msg ("bvec is wrong");
			ret = -1;
			break;
		}

		/* get a destination page */
		dst_page = bvec.bv_page;

		/* get a mapped phyiscal page */
		lblkaddr = dst_page->index;
		pblkaddr = amf_get_mapped_pblkaddr (sbi, lblkaddr);
		if (pblkaddr == NULL_ADDR) {
			ret = -1;
			goto out;
		}
		//pr_notice("lblkaddr = %d ==> pblkaddr = %d\n", lblkaddr, pblkaddr);

		/* read the requested page */
		if (tgt_submit_page_read_sync(sbi, src_page, pblkaddr) != 0) {
			amf_dbg_msg ("amf_read_page_flash failed");
			ret = -1;
			goto out;
		}

		/* copy memory data */
		src_page_addr = (uint8_t*)page_address (src_page);
		dst_page_addr = (uint8_t*)page_address (dst_page);
		memcpy (dst_page_addr, src_page_addr, PAGE_SIZE);

		/* go to the next page */
		ClearPageUptodate (src_page);
	}

out:
	/* unlock & free the page */
	unlock_page (src_page);
	__free_pages (src_page, 0);


	if (ret == 0) {
		//bio_clear_flag(bio, BIO_TRACE_COMPLETION);	
		//pr_notice("invoke bio->bi_end_io()\n");
		bio->bi_end_io (bio);
	} else {
		bio->bi_end_io (bio);
	}
}

void amf_submit_bio (struct f2fs_sb_info* sbi, struct bio * bio, enum page_type type)
{

	block_t lblkaddr = amf_get_lba(bio) ;

	if (amf_is_cp_blk (sbi, lblkaddr)) {
	amf_dbg_msg("lblkaddr = %d enter amf_is_cp_blk()\n", lblkaddr);
		amf_write_mapping_entries (sbi);
#ifdef AMF_PMU
			atomic64_inc (&sbi->pmu.ckp_w);
#endif
	}

	if (is_valid_meta_lblkaddr (sbi, lblkaddr) == 0) {
		//pr_notice("is_valid_meta_lblkaddr() is true\n");
		if (op_is_write(bio_op(bio))) {//write
#ifdef AMF_PMU
			atomic64_add(bio_sectors (bio) / 8, &sbi->pmu.meta_w);
#endif
			amf_submit_bio_meta_w(sbi, bio);
		}else if(is_read_io(bio_op(bio))){
#ifdef AMF_PMU
			atomic64_add(bio_sectors (bio) / 8, &sbi->pmu.meta_r);
#endif
			amf_submit_bio_meta_r(sbi, bio);
		} else {
			amf_dbg_msg("[WARNING] unknown type: %d", bio_op(bio));
			submit_bio(bio);
		}		
	}else{//其他数据的读写
#ifdef AMF_PMU
		if (op_is_write(bio_op(bio))) {
			atomic64_add(bio_sectors (bio) / 8, &sbi->pmu.norm_w);
		} else {
			atomic64_add(bio_sectors (bio) / 8, &sbi->pmu.norm_r);
		}
#endif
		if (!is_read_io(bio_op(bio))) {//非read
			unsigned int start;
			if (f2fs_sb_mounted_blkzoned(sbi->sb) &&
				current->plug && (type == DATA || type == NODE))
				blk_finish_plug(current->plug);
	
			if (type != DATA && type != NODE)
				goto submit_io;
	
			start = bio->bi_iter.bi_size >> F2FS_BLKSIZE_BITS;
			start %= F2FS_IO_SIZE(sbi);
	//pr_notice("bio->bi_iter.bi_size = %d\n", bio->bi_iter.bi_size);
	//pr_notice("start = %d\n", start);
			if (start == 0)
				goto submit_io;
	
			/* fill dummy pages */
			for (; start < F2FS_IO_SIZE(sbi); start++) {//F2FS_IO_SIZE = 1
				struct page *page =
					mempool_alloc(sbi->write_io_dummy,
						GFP_NOIO | __GFP_ZERO | __GFP_NOFAIL);
				f2fs_bug_on(sbi, !page);
	
				SetPagePrivate(page);
				set_page_private(page, (unsigned long)DUMMY_WRITTEN_PAGE);
				lock_page(page);
				if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE)
					f2fs_bug_on(sbi, 1);
			}
			/*
			 * In the NODE case, we lose next block address chain. So, we
			 * need to do checkpoint in f2fs_sync_file.
			 */
			if (type == NODE)
				set_sbi_flag(sbi, SBI_NEED_CP);
		}
submit_io:
		if (is_read_io(bio_op(bio)))
			trace_f2fs_submit_read_bio(sbi->sb, type, bio);
		else
			trace_f2fs_submit_write_bio(sbi->sb, type, bio);
	
#ifdef AMF_META_LOGGING
		if(is_read_io(bio_op(bio))){
			amf_submit_bio_read_sync(sbi, bio);
		}else{
			amf_submit_bio_write(sbi, bio);			
		}
#else
#ifdef AMF_PMU
		if(!is_read_io(bio)){
			atomic64_add(bio_sectors(bio)/8, &sbi->pmu.norm_w);
		}else{
			atomic64_add(bio_sectors(bio)/8, &sbi->pmu.norm_r);
		}
#endif
		submit_bio(bio);
		
#endif
}	

}


int32_t get_metalog_free_blks (struct f2fs_sb_info* sbi)
{
	struct amf_info* ri = AMF_RI (sbi);
	uint32_t nr_free_blks;

	if (ri->metalog_gc_sblkofs < ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->nr_metalog_phys_blks - ri->metalog_gc_eblkofs + ri->metalog_gc_sblkofs;
	} else if (ri->metalog_gc_sblkofs > ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->metalog_gc_sblkofs - ri->metalog_gc_eblkofs;
	} else {
		amf_dbg_msg ("[ERROR] 'ri->metalog_gc_sblkofs (%u)' is equal to 'ri->metalog_gc_eblkofs (%u)'", 
			ri->metalog_gc_sblkofs, ri->metalog_gc_eblkofs);
		nr_free_blks = -1;
	}
//pr_notice("nr_free_blks = %d\n",nr_free_blks);
	return nr_free_blks;
}


int8_t is_gc_needed (struct f2fs_sb_info* sbi, int32_t nr_free_blks)
{
	struct amf_info* ri = AMF_RI (sbi);
	mutex_lock (&ri->amf_gc_mutex); 
	if (nr_free_blks <= (sbi->segs_per_sec * sbi->blocks_per_seg)) {
		mutex_unlock (&ri->amf_gc_mutex); 
		return 0;
	}

	mutex_unlock (&ri->amf_gc_mutex); 
	return -1;
}

uint32_t amf_get_mapped_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr)
{

	struct amf_info* ri = AMF_RI (sbi);
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
			amf_msg ("invalid pblkaddr: (%llu (=%llu-%llu) => %u)", 
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
		amf_dbg_msg ("the summary table is incorrect: pblkaddr=%u (%u)",
			pblkaddr, ri->summary_table[pblkaddr - ri->metalog_blkofs]);
	}

	return pblkaddr;
}


//没用
int8_t amf_do_trim (struct f2fs_sb_info* sbi, block_t pblkaddr, uint32_t nr_blks)
{
	if (test_opt (sbi, DISCARD)) {
		blkdev_issue_discard (
			sbi->sb->s_bdev, 
			SECTOR_FROM_BLOCK (pblkaddr), 
			nr_blks * 8, 
			GFP_NOFS, 
			0);
		return 0;
	}
	return -1;
}

int8_t amf_do_gc (struct f2fs_sb_info* sbi)//应该是metalog的gc
{
	struct amf_info* ri = AMF_RI (sbi);
	struct page* page = NULL;

	uint32_t cur_blkofs = 0;
	uint32_t loop = 0;

	mutex_lock (&ri->amf_gc_mutex); 

	/* see if ri is initialized or not */
	if (sbi->ri == NULL) {
		mutex_unlock(&ri->amf_gc_mutex); 
		return -1;
	}

	/* check the alignment */
	if (ri->metalog_gc_sblkofs % (sbi->segs_per_sec * sbi->blocks_per_seg) != 0) {
		amf_dbg_msg ("ri->metalog_gc_sblkofs %% sbi->blocks_per_seg != 0 (%u)", 
			ri->metalog_gc_sblkofs % (sbi->segs_per_sec * sbi->blocks_per_seg));
		mutex_unlock(&ri->amf_gc_mutex); 
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
			//或者用lightnvm的erase
			/*if (f2fs_issue_discard (sbi, ri->metalog_blkofs + cur_blkofs, 1) == -1) {
				amf_dbg_msg (KERN_INFO "Errors occur while trimming the page during GC");
			}*/

			/*if (tgt_submit_addr_erase_async(sbi, ri->metalog_blkofs + cur_blkofs, 1) == -1) {
				amf_dbg_msg (KERN_INFO "Errors occur while trimming the page during GC");
			}*/
			continue;
		}

		/* allocate a new page (This page is released later by 'risa_end_io_flash' */
		page = alloc_page (GFP_NOFS | __GFP_ZERO);
		if (IS_ERR (page)) {
			amf_dbg_msg ("page is invalid");
			mutex_unlock(&ri->amf_gc_mutex); 
			return PTR_ERR (page);
		}
		lock_page (page);

		/* determine src & dst blks */
		src_pblkaddr = ri->metalog_blkofs + cur_blkofs;
		dst_pblkaddr = ri->metalog_blkofs + ri->metalog_gc_eblkofs;

		/* read the valid blk into the page */
		if (amf_readpage (sbi, page, src_pblkaddr) == -1) {
			printk (KERN_INFO "[ERROR] errors occur while reading the page during GC");
			continue;
		}

		/* write the valid pages into the free segment */
		if (amf_writepage (sbi, page, dst_pblkaddr, 0) == -1) {
			printk (KERN_INFO "[ERROR] errors occur while writing the page during GC");
			continue;
		}

		/* trim the source page */
		/*if (f2fs_issue_discard (sbi, src_pblkaddr, 1) == -1) {
			printk (KERN_INFO "[ERROR] errors occur while trimming the page during GC");
			continue;
		}*/

		/*if (tgt_submit_addr_erase_async (sbi, src_pblkaddr, 1) == -1) {
			printk (KERN_INFO "[ERROR] errors occur while trimming the page during GC");
			continue;
		}
		*/

		
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

#ifdef AMF_PMU
		atomic64_add (1, &sbi->pmu.metalog_gc_rw);
#endif
	}
	/*trim 整个segment*/
	if (tgt_submit_addr_erase_async (sbi, ri->metalog_gc_sblkofs, sbi->segs_per_sec * sbi->blocks_per_seg) == -1) {
		printk (KERN_INFO "[ERROR] errors occur while trimming the page during GC");
	}
	/* update start offset */
	ri->metalog_gc_sblkofs = 
		(ri->metalog_gc_sblkofs + (sbi->segs_per_sec * sbi->blocks_per_seg)) % 
		ri->nr_metalog_phys_blks;

	mutex_unlock (&ri->amf_gc_mutex); 

	return 0;
}

