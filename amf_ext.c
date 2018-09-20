#include <linux/fs.h>
#include <lilnux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>

#include "f2fs.h"
#include "amf_ext.h"
#include "segment.h"


/*
 * 处理读写操作
 */
 static void amf_end_io_flash(struct bio* bio, int err){
	struct amf_bio_private *p = bio->bi_private;
	if(p->page){
		ClearPageUptodata(p->page);
		unlock_page(page);
		__free_pages(p->page, 0);
	}

	if(p->is_sync)
		complete(p->wait);

	kfree(p);
	bio_put(bio);
 }

 static int32_t amf_read_flash(struct f2fs_sb_info* sbi, struct page* page, block_t blkaddr){
	struct block_device* bdev = sbi->sb->s_bdev;
	struct bio* bio = NULL;
	struct amf_bio_private* p=NULL;

	DECLARE_COMPLETION_ONSTACK(wait);

retry:
	p = kmalloc(sizeof(struct amf_bio_private), GFP_NOFS);
	if(!p){
		cond_resched();
		goto retry;
	}


	/* allocate a new bio */
	bio = f2fs_bio_alloc(sbi, 1, true);

	/* initialize the bio */
	bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(blk_addr);
	bio->bi_end_io = amf_end_io_flash;

	p->sbi = sbi;
	p->page = NULL;
	bio->bi_private = p;

	/* put a bio into a bio queue */
	if(bio_add_page(bio, page,  PAGE_SIZE, 0) < PAGE_SIZE)){
		amf_dbg_msg("Error occur while calling amf_readpage");
		kfree(bio->bi_private);
		bio_put(bio);
		return -EFAULT;
	}

	/* submit a bio request to a device */
	p->is_sync = true;
	p->wait = &wait;
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	submit_bio(bio);
	wait_for_completion(&wait);

	/* see if page is correct or not*/
	if(PageError(page))
		return -EIO;

	return 0;
 }

 static int32_t amf_writepage_flash(struct f2fs_sb_info* sbi, struct page* page, block_t blkaddr, unit8_t sync){
	struct block_device* bdev = sbi->sb->s_bdev;
	struct bio* bio = NULL;
	struct amf_bio_private* p = NULL;

	DECLARE_COMPLETION_ONSTACK (wait);

retry:
	p = kmalloc(sizeof(struct amf_bio_private). GFP_NOFS);
	if(!p){
		cond_resched();
		goto retry;
	}

	/* allocate a new bio */
	bio = f2fs_bio_alloc(sbi, 1, true);

	/* initialize the bio */
	bio->bi_iter->bi_sector = SECTOR_FROM_BLOCK (blkaddr);
	bio->bi_end_io = amf_end_io_flash;

	p->sbi = sbi;
	p->page = page;
	bio->bi_private = p;

	/* put a bio into a bio queue */
	if(bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE){
		amf_dbg_msg("Error occur while calling amf_writepage");
		kfree(bio->bi_private);
		bio_put(bio);
		return -EFAULT;
	}

	/* submit a bio request to a device */
	if(sync == 1){
		p->is_sync = true;
		p->wait = &wait;
	}else{
		p->is_sync = false;
	}
	bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
	if(sync == 1)
		wait_for_completion(&wait);

	/* see if page is correct or not */
	if(PageError(page))
		return -EIO;

	return 0;	
 }

