/*
 * Copyright (C) 2015 IT University of Copenhagen (rrpc.c)
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Javier Gonzalez <javier@cnexlabs.com>
 *                  Matias Bjorling <matias@cnexlabs.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Implementation of a physical block-device target for Open-channel SSDs.
 *
 * pblk-init.c - pblk's initialization.
 */

#include "pblk.h"

static struct kmem_cache *pblk_ws_cache, *pblk_rec_cache, *pblk_g_rq_cache,
				*pblk_w_rq_cache;
static DECLARE_RWSEM(pblk_lock);
struct bio_set *pblk_bio_set;

static int pblk_rw_io(struct request_queue *q, struct pblk *pblk,
			  struct bio *bio)
{
	int ret;

	/* Read requests must be <= 256kb due to NVMe's 64 bit completion bitmap
	 * constraint. Writes can be of arbitrary size.
	 */
	if (bio_data_dir(bio) == READ) {
		blk_queue_split(q, &bio);
		ret = pblk_submit_read(pblk, bio);
		if (ret == NVM_IO_DONE && bio_flagged(bio, BIO_CLONED))
			bio_put(bio);

		return ret;
	}

	/* Prevent deadlock in the case of a modest LUN configuration and large
	 * user I/Os. Unless stalled, the rate limiter leaves at least 256KB
	 * available for user I/O.
	 */
	if (pblk_get_secs(bio) > pblk_rl_max_io(&pblk->rl))
		blk_queue_split(q, &bio);

	return pblk_write_to_cache(pblk, bio, PBLK_IOTYPE_USER);
}

static blk_qc_t pblk_make_rq(struct request_queue *q, struct bio *bio)
{
	struct pblk *pblk = q->queuedata;

	if (bio_op(bio) == REQ_OP_DISCARD) {
		pblk_discard(pblk, bio);
		if (!(bio->bi_opf & REQ_PREFLUSH)) {
			bio_endio(bio);
			return BLK_QC_T_NONE;
		}
	}

	switch (pblk_rw_io(q, pblk, bio)) {
	case NVM_IO_ERR:
		bio_io_error(bio);
		break;
	case NVM_IO_DONE:
		bio_endio(bio);
		break;
	}

	return BLK_QC_T_NONE;
}

static size_t pblk_trans_map_size(struct pblk *pblk)
{
	int entry_size = 8;

	if (pblk->ppaf_bitsize < 32)
		entry_size = 4;

	return entry_size * pblk->rl.nr_secs;
}

#ifdef CONFIG_NVM_DEBUG
static u32 pblk_l2p_crc(struct pblk *pblk)
{
	size_t map_size;
	u32 crc = ~(u32)0;

	map_size = pblk_trans_map_size(pblk);
	crc = crc32_le(crc, pblk->trans_map, map_size);
	return crc;
}
#endif

static void pblk_l2p_free(struct pblk *pblk)
{
	vfree(pblk->trans_map);
}

static int pblk_l2p_init(struct pblk *pblk)
{
	sector_t i;
	struct ppa_addr ppa;
	size_t map_size;

	map_size = pblk_trans_map_size(pblk);
	pblk->trans_map = vmalloc(map_size);
	if (!pblk->trans_map)
		return -ENOMEM;

	pblk_ppa_set_empty(&ppa);

	for (i = 0; i < pblk->rl.nr_secs; i++)
		pblk_trans_map_set(pblk, i, ppa);

	return 0;
}

static void pblk_rwb_free(struct pblk *pblk)
{
	if (pblk_rb_tear_down_check(&pblk->rwb))
		pr_err("pblk: write buffer error on tear down\n");

	pblk_rb_data_free(&pblk->rwb);
	vfree(pblk_rb_entries_ref(&pblk->rwb));
}

static int pblk_rwb_init(struct pblk *pblk)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_rb_entry *entries;
	unsigned long nr_entries;
	unsigned int power_size, power_seg_sz;

	nr_entries = pblk_rb_calculate_size(pblk->pgs_in_buffer);

	entries = vzalloc(nr_entries * sizeof(struct pblk_rb_entry));
	if (!entries)
		return -ENOMEM;

	power_size = get_count_order(nr_entries);
	power_seg_sz = get_count_order(geo->sec_size);

	return pblk_rb_init(&pblk->rwb, entries, power_size, power_seg_sz);
}

/* Minimum pages needed within a lun */
#define ADDR_POOL_SIZE 64

static int pblk_set_ppaf(struct pblk *pblk)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct nvm_addr_format ppaf = geo->ppaf;
	int power_len;

	/* Re-calculate channel and lun format to adapt to configuration */
	power_len = get_count_order(geo->nr_chnls);
	if (1 << power_len != geo->nr_chnls) {
		pr_err("pblk: supports only power-of-two channel config.\n");
		return -EINVAL;
	}
	ppaf.ch_len = power_len;

	power_len = get_count_order(geo->nr_luns);
	if (1 << power_len != geo->nr_luns) {
		pr_err("pblk: supports only power-of-two LUN config.\n");
		return -EINVAL;
	}
	ppaf.lun_len = power_len;

	pblk->ppaf.sec_offset = 0;
	pblk->ppaf.pln_offset = ppaf.sect_len;
	pblk->ppaf.ch_offset = pblk->ppaf.pln_offset + ppaf.pln_len;
	pblk->ppaf.lun_offset = pblk->ppaf.ch_offset + ppaf.ch_len;
	pblk->ppaf.pg_offset = pblk->ppaf.lun_offset + ppaf.lun_len;
	pblk->ppaf.blk_offset = pblk->ppaf.pg_offset + ppaf.pg_len;
	pblk->ppaf.sec_mask = (1ULL << ppaf.sect_len) - 1;
	pblk->ppaf.pln_mask = ((1ULL << ppaf.pln_len) - 1) <<
							pblk->ppaf.pln_offset;
	pblk->ppaf.ch_mask = ((1ULL << ppaf.ch_len) - 1) <<
							pblk->ppaf.ch_offset;
	pblk->ppaf.lun_mask = ((1ULL << ppaf.lun_len) - 1) <<
							pblk->ppaf.lun_offset;
	pblk->ppaf.pg_mask = ((1ULL << ppaf.pg_len) - 1) <<
							pblk->ppaf.pg_offset;
	pblk->ppaf.blk_mask = ((1ULL << ppaf.blk_len) - 1) <<
							pblk->ppaf.blk_offset;

	pblk->ppaf_bitsize = pblk->ppaf.blk_offset + ppaf.blk_len;
/*pr_notice("pblk->ppaf.sec_offset=0x%x\npblk->ppaf.pln_offset=0x%x\npblk->ppaf.ch_offset=0x%x\npblk->ppaf.lun_offset =0x%x\n \
	pblk->ppaf.pg_offset=0x%x\npblk->ppaf.blk_offset=0x%x\npblk->ppaf.sec_mask=0x%x\npblk->ppaf.pln_mask=0x%x\npblk->ppaf.ch_mask=0x%x\n \
	pblk->ppaf.lun_mask=0x%x\npblk->ppaf.pg_mask=0x%x\npblk->ppaf.blk_mask=0x%x\n",
	pblk->ppaf.sec_offset,pblk->ppaf.pln_offset,pblk->ppaf.ch_offset,pblk->ppaf.lun_offset,pblk->ppaf.pg_offset,
	pblk->ppaf.blk_offset,pblk->ppaf.sec_mask,pblk->ppaf.pln_mask,pblk->ppaf.ch_mask,pblk->ppaf.lun_mask,pblk->ppaf.pg_mask,
	pblk->ppaf.blk_mask);
pr_notice("pblk->ppaf_bitsize=0x%x\n",pblk->ppaf_bitsize);
*/
	return 0;
}

static int pblk_init_global_caches(struct pblk *pblk)
{
	down_write(&pblk_lock);
	//kmem_cache_create()用于创建专用高速缓存。
	pblk_ws_cache = kmem_cache_create("pblk_blk_ws",
				sizeof(struct pblk_line_ws), 0, 0, NULL);
	if (!pblk_ws_cache) {
		up_write(&pblk_lock);
		return -ENOMEM;
	}

	pblk_rec_cache = kmem_cache_create("pblk_rec",
				sizeof(struct pblk_rec_ctx), 0, 0, NULL);
	if (!pblk_rec_cache) {
		kmem_cache_destroy(pblk_ws_cache);
		up_write(&pblk_lock);
		return -ENOMEM;
	}

	pblk_g_rq_cache = kmem_cache_create("pblk_g_rq", pblk_g_rq_size,
				0, 0, NULL);
	if (!pblk_g_rq_cache) {
		kmem_cache_destroy(pblk_ws_cache);
		kmem_cache_destroy(pblk_rec_cache);
		up_write(&pblk_lock);
		return -ENOMEM;
	}

	pblk_w_rq_cache = kmem_cache_create("pblk_w_rq", pblk_w_rq_size,
				0, 0, NULL);
	if (!pblk_w_rq_cache) {
		kmem_cache_destroy(pblk_ws_cache);
		kmem_cache_destroy(pblk_rec_cache);
		kmem_cache_destroy(pblk_g_rq_cache);
		up_write(&pblk_lock);
		return -ENOMEM;
	}
	up_write(&pblk_lock);

	return 0;
}

static void pblk_free_global_caches(struct pblk *pblk)
{
	kmem_cache_destroy(pblk_ws_cache);
	kmem_cache_destroy(pblk_rec_cache);
	kmem_cache_destroy(pblk_g_rq_cache);
	kmem_cache_destroy(pblk_w_rq_cache);
}

static int pblk_core_init(struct pblk *pblk)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;

	pblk->pgs_in_buffer = NVM_MEM_PAGE_WRITE * geo->sec_per_pg *
						geo->nr_planes * geo->all_luns;//0x100=256

	if (pblk_init_global_caches(pblk))
		return -ENOMEM;

	/* Internal bios can be at most the sectors signaled by the device. */
	pblk->page_bio_pool = mempool_create_page_pool(nvm_max_phys_sects(dev),
									0);
	if (!pblk->page_bio_pool)
		goto free_global_caches;

	pblk->gen_ws_pool = mempool_create_slab_pool(PBLK_GEN_WS_POOL_SIZE,
							pblk_ws_cache);
	if (!pblk->gen_ws_pool)
		goto free_page_bio_pool;

	pblk->rec_pool = mempool_create_slab_pool(geo->all_luns,
							pblk_rec_cache);
	if (!pblk->rec_pool)
		goto free_gen_ws_pool;

	pblk->r_rq_pool = mempool_create_slab_pool(geo->all_luns,
							pblk_g_rq_cache);
	if (!pblk->r_rq_pool)
		goto free_rec_pool;

	pblk->e_rq_pool = mempool_create_slab_pool(geo->all_luns,
							pblk_g_rq_cache);
	if (!pblk->e_rq_pool)
		goto free_r_rq_pool;

	pblk->w_rq_pool = mempool_create_slab_pool(geo->all_luns,
							pblk_w_rq_cache);
	if (!pblk->w_rq_pool)
		goto free_e_rq_pool;

	pblk->close_wq = alloc_workqueue("pblk-close-wq",
			WQ_MEM_RECLAIM | WQ_UNBOUND, PBLK_NR_CLOSE_JOBS);
	if (!pblk->close_wq)
		goto free_w_rq_pool;

	pblk->bb_wq = alloc_workqueue("pblk-bb-wq",
			WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	if (!pblk->bb_wq)
		goto free_close_wq;

	pblk->r_end_wq = alloc_workqueue("pblk-read-end-wq",
			WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	if (!pblk->r_end_wq)
		goto free_bb_wq;

	if (pblk_set_ppaf(pblk))
		goto free_r_end_wq;

	if (pblk_rwb_init(pblk))
		goto free_r_end_wq;

	INIT_LIST_HEAD(&pblk->compl_list);
	return 0;

free_r_end_wq:
	destroy_workqueue(pblk->r_end_wq);
free_bb_wq:
	destroy_workqueue(pblk->bb_wq);
free_close_wq:
	destroy_workqueue(pblk->close_wq);
free_w_rq_pool:
	mempool_destroy(pblk->w_rq_pool);
free_e_rq_pool:
	mempool_destroy(pblk->e_rq_pool);
free_r_rq_pool:
	mempool_destroy(pblk->r_rq_pool);
free_rec_pool:
	mempool_destroy(pblk->rec_pool);
free_gen_ws_pool:
	mempool_destroy(pblk->gen_ws_pool);
free_page_bio_pool:
	mempool_destroy(pblk->page_bio_pool);
free_global_caches:
	pblk_free_global_caches(pblk);
	return -ENOMEM;
}

static void pblk_core_free(struct pblk *pblk)
{
	if (pblk->close_wq)
		destroy_workqueue(pblk->close_wq);

	if (pblk->r_end_wq)
		destroy_workqueue(pblk->r_end_wq);

	if (pblk->bb_wq)
		destroy_workqueue(pblk->bb_wq);

	mempool_destroy(pblk->page_bio_pool);
	mempool_destroy(pblk->gen_ws_pool);
	mempool_destroy(pblk->rec_pool);
	mempool_destroy(pblk->r_rq_pool);
	mempool_destroy(pblk->e_rq_pool);
	mempool_destroy(pblk->w_rq_pool);

	pblk_rwb_free(pblk);

	pblk_free_global_caches(pblk);
}

static void pblk_luns_free(struct pblk *pblk)
{
	kfree(pblk->luns);
}

static void pblk_free_line_bitmaps(struct pblk_line *line)
{
	kfree(line->blk_bitmap);
	kfree(line->erase_bitmap);
}

static void pblk_lines_free(struct pblk *pblk)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line *line;
	int i;

	spin_lock(&l_mg->free_lock);
	for (i = 0; i < l_mg->nr_lines; i++) {
		line = &pblk->lines[i];

		pblk_line_free(pblk, line);
		pblk_free_line_bitmaps(line);
	}
	spin_unlock(&l_mg->free_lock);
}

static void pblk_line_meta_free(struct pblk *pblk)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	int i;

	kfree(l_mg->bb_template);
	kfree(l_mg->bb_aux);
	kfree(l_mg->vsc_list);

	for (i = 0; i < PBLK_DATA_LINES; i++) {
		kfree(l_mg->sline_meta[i]);
		pblk_mfree(l_mg->eline_meta[i]->buf, l_mg->emeta_alloc_type);
		kfree(l_mg->eline_meta[i]);
	}

	kfree(pblk->lines);
}

static int pblk_bb_discovery(struct nvm_tgt_dev *dev, struct pblk_lun *rlun)
{
	struct nvm_geo *geo = &dev->geo;
	struct ppa_addr ppa;
	u8 *blks;
	int nr_blks, ret;

	nr_blks = geo->nr_chks * geo->plane_mode;//=126
//pr_notice("nr_blks=0x%x\n",nr_blks);
	blks = kmalloc(nr_blks, GFP_KERNEL);
	if (!blks)
		return -ENOMEM;

	ppa.ppa = 0;
	ppa.g.ch = rlun->bppa.g.ch;
	ppa.g.lun = rlun->bppa.g.lun;

	ret = nvm_get_tgt_bb_tbl(dev, ppa, blks);//从设备中得到的bb_tbl信息存入blks中。
	if (ret)
		goto out;

	nr_blks = nvm_bb_tbl_fold(dev->parent, blks, nr_blks);//用第一个plane的bb状态代表整个plane中bb状态，仅读取plane中第一个blk状态即可
	if (nr_blks < 0) {
		ret = nr_blks;
		goto out;
	}

	rlun->bb_list = blks;

	return 0;
out:
	kfree(blks);
	return ret;
}

static int pblk_bb_line(struct pblk *pblk, struct pblk_line *line,
			int blk_per_line)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_lun *rlun;
	int bb_cnt = 0;
	int i;

	for (i = 0; i < blk_per_line; i++) {
		rlun = &pblk->luns[i];
		if (rlun->bb_list[line->id] == NVM_BLK_T_FREE)
			continue;

		set_bit(pblk_ppa_to_pos(geo, rlun->bppa), line->blk_bitmap);//pblk_ppa_to_pos=0,1,2,3
		bb_cnt++;
	}

	return bb_cnt;
}

static int pblk_alloc_line_bitmaps(struct pblk *pblk, struct pblk_line *line)
{
	struct pblk_line_meta *lm = &pblk->lm;

	line->blk_bitmap = kzalloc(lm->blk_bitmap_len, GFP_KERNEL);
	if (!line->blk_bitmap)
		return -ENOMEM;

	line->erase_bitmap = kzalloc(lm->blk_bitmap_len, GFP_KERNEL);
	if (!line->erase_bitmap) {
		kfree(line->blk_bitmap);
		return -ENOMEM;
	}

	return 0;
}

static int pblk_luns_init(struct pblk *pblk, struct ppa_addr *luns)
{//每个lun从设备中读取bb_tbl并将其存入luns[]->bb_list中。
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_lun *rlun;
	int i, ret;
//pr_notice("Enter pblk_luns_init()\n");

	/* TODO: Implement unbalanced LUN support */
	if (geo->nr_luns < 0) {
		pr_err("pblk: unbalanced LUN config.\n");
		return -EINVAL;
	}
/*pr_notice("dev->geo:\n");
pr_notice("geo->nr_chnls=0x%x, geo->all_luns=0x%x, geo->nr_luns=0x%x, geo->nr_chks=0x%x, geo->sec_size=0x%x, geo->oob_size=0x%x, \
	geo->mccap=0x%x, geo->sec_per_chk=0x%x, geo->sec_per_lun=0x%x, geo->ws_min=0x%x, geo->ws_opt=0x%x, geo->ws_seq=0x%x, \
	geo->ws_per_chk=0x%x, geo->max_rq_size=0x%x, geo->op=0x%x, geo->plane_mode=0x%x, geo->nr_planes=0x%x, geo->sec_per_pg=0x%x, \
	geo->sec_per_pl=0x%x\n",geo->nr_chnls,geo->all_luns,geo->nr_luns,geo->nr_chks,geo->sec_size,geo->oob_size,geo->mccap,
	geo->sec_per_chk,geo->sec_per_lun,geo->ws_min,geo->ws_opt,geo->ws_seq,geo->ws_per_chk,geo->max_rq_size,
	geo->op,geo->plane_mode,geo->nr_planes,geo->sec_per_pg,geo->sec_per_pl);
struct nvm_addr_format geo_ppaf = geo->ppaf;
pr_notice("dev->geo->ppaf:\n");
pr_notice("geo_ppaf.ch_offset=0x%x, geo_ppaf.ch_len=0x%x, geo_ppaf.lun_offset=0x%x, geo_ppaf.lun_len=0x%x, geo_ppaf.pln_offset=0x%x, \
	geo_ppaf.pln_len=0x%x, geo_ppaf.blk_offset=0x%x, geo_ppaf.blk_len=0x%x, geo_ppaf.pg_offset=0x%x, geo_ppaf.pg_len=0x%x, \
	geo_ppaf.sect_offset=0x%x, geo_ppaf.sect_len=0x%x\n",geo_ppaf.ch_offset,geo_ppaf.ch_len,geo_ppaf.lun_offset,geo_ppaf.lun_len,geo_ppaf.pln_offset,geo_ppaf.pln_len,
	geo_ppaf.blk_offset,geo_ppaf.blk_len,geo_ppaf.pg_offset,geo_ppaf.pg_len,geo_ppaf.sect_offset,geo_ppaf.sect_len);
*/

/*
geo->nr_chnls=0x1, 
geo->all_luns=0x4, geo->nr_luns=0x4, 
geo->nr_chks=0x3f=63, 
geo->sec_size=0x1000=4096, 
geo->oob_size=0x10=16,
geo->mccap=0x1, 
geo->sec_per_chk=0x800=2048,
geo->sec_per_lun=0x1f800=‭129024‬,
geo->ws_min=0x4, geo->ws_opt=0x8, geo->ws_seq=0x1,
geo->ws_per_chk=0x100=256, geo->max_rq_size=0x40000=‭262144‬, geo->op=0x65=‭01100101‬, geo->plane_mode=0x2, geo->nr_planes=0x2, 
geo->sec_per_pg=0x4,
geo->sec_per_pl=0x8

geo_ppaf.ch_offset=0x14, geo_ppaf.ch_len=0x0, geo_ppaf.lun_offset=0x11, geo_ppaf.lun_len=0x3, geo_ppaf.pln_offset=0x2,
geo_ppaf.pln_len=0x1, geo_ppaf.blk_offset=0xb, geo_ppaf.blk_len=0x6, geo_ppaf.pg_offset=0x3, geo_ppaf.pg_len=0x8,
geo_ppaf.sect_offset=0x0, geo_ppaf.sect_len=0x2

重要的几个：


pblk:pblk_init:1101: pblk(mypblk): luns:4, lines:63, secs:516096, buf entries:256
pblk->capacity=0x6f800=‭456704‬, NR_PHY_IN_LOG=0x8

*/
	pblk->luns = kcalloc(geo->all_luns, sizeof(struct pblk_lun),
								GFP_KERNEL);
	if (!pblk->luns)
		return -ENOMEM;

	for (i = 0; i < geo->all_luns; i++) {
		/* Stripe across channels */
		int ch = i % geo->nr_chnls;
		int lun_raw = i / geo->nr_chnls;
		int lunid = lun_raw + ch * geo->nr_luns;
//pr_notice("lunid=0x%x\n",lunid);
		rlun = &pblk->luns[i];
		rlun->bppa = luns[lunid];

		sema_init(&rlun->wr_sem, 1);

		ret = pblk_bb_discovery(dev, rlun);//从设备得到每个lun的bb_tbl并存入rlun中
		if (ret) {
			while (--i >= 0)
				kfree(pblk->luns[i].bb_list);
			return ret;
		}
	}

	return 0;
}

static int pblk_lines_configure(struct pblk *pblk, int flags)
{
	struct pblk_line *line = NULL;
	int ret = 0;

	if (!(flags & NVM_TARGET_FACTORY)) {//flags = 0x0
		line = pblk_recov_l2p(pblk);
		if (IS_ERR(line)) {
			pr_err("pblk: could not recover l2p table\n");
			ret = -EFAULT;
		}
	}

#ifdef CONFIG_NVM_DEBUG
	pr_info("pblk init: L2P CRC: %x\n", pblk_l2p_crc(pblk));
#endif

	/* Free full lines directly as GC has not been started yet */
	pblk_gc_free_full_lines(pblk);

	if (!line) {
		/* Configure next line for user data */
//pr_notice("!line is true.\n");
		line = pblk_line_get_first_data(pblk);
		if (!line) {
			pr_err("pblk: line list corrupted\n");
			ret = -EFAULT;
		}
	}

	return ret;
}

/* See comment over struct line_emeta definition */
static unsigned int calc_emeta_len(struct pblk *pblk)
{
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;

	/* Round to sector size so that lba_list starts on its own sector */
	lm->emeta_sec[1] = DIV_ROUND_UP(
			sizeof(struct line_emeta) + lm->blk_bitmap_len,
			geo->sec_size);
	lm->emeta_len[1] = lm->emeta_sec[1] * geo->sec_size;

	/* Round to sector size so that vsc_list starts on its own sector */
	lm->dsec_per_line = lm->sec_per_line - lm->emeta_sec[0];
	lm->emeta_sec[2] = DIV_ROUND_UP(lm->dsec_per_line * sizeof(u64),
			geo->sec_size);
	lm->emeta_len[2] = lm->emeta_sec[2] * geo->sec_size;

	lm->emeta_sec[3] = DIV_ROUND_UP(l_mg->nr_lines * sizeof(u32),
			geo->sec_size);
	lm->emeta_len[3] = lm->emeta_sec[3] * geo->sec_size;

	lm->vsc_list_len = l_mg->nr_lines * sizeof(u32);

	return (lm->emeta_len[1] + lm->emeta_len[2] + lm->emeta_len[3]);
}

static void pblk_set_provision(struct pblk *pblk, long nr_free_blks)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	struct nvm_geo *geo = &dev->geo;
	sector_t provisioned;
	int sec_meta, blk_meta;

	if (geo->op == NVM_TARGET_DEFAULT_OP)
		pblk->op = PBLK_DEFAULT_OP;
	else
		pblk->op = geo->op;

	provisioned = nr_free_blks;
	provisioned *= (100 - pblk->op);//pblk->op=11 provisioned = ‭22428‬
	sector_div(provisioned, 100);

	pblk->op_blks = nr_free_blks - provisioned;//28

	/* Internally pblk manages all free blocks, but all calculations based
	 * on user capacity consider only provisioned blocks
	 */
	pblk->rl.total_blocks = nr_free_blks;//252
	pblk->rl.nr_secs = nr_free_blks * geo->sec_per_chk;

	/* Consider sectors used for metadata */
	sec_meta = (lm->smeta_sec + lm->emeta_sec[0]) * l_mg->nr_free_lines;//sec_meta=0x7e0=2016
	blk_meta = DIV_ROUND_UP(sec_meta, geo->sec_per_chk);//blk_meta=1

	pblk->capacity = (provisioned - blk_meta) * geo->sec_per_chk;//0x6f800

	atomic_set(&pblk->rl.free_blocks, nr_free_blks);//0xfc=252
	atomic_set(&pblk->rl.free_user_blocks, nr_free_blks);
}

static int pblk_lines_alloc_metadata(struct pblk *pblk)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	int i;

	/* smeta is always small enough to fit on a kmalloc memory allocation,
	 * emeta depends on the number of LUNs allocated to the pblk instance
	 */
	for (i = 0; i < PBLK_DATA_LINES; i++) {
		l_mg->sline_meta[i] = kmalloc(lm->smeta_len, GFP_KERNEL);
		if (!l_mg->sline_meta[i])
			goto fail_free_smeta;
	}

	/* emeta allocates three different buffers for managing metadata with
	 * in-memory and in-media layouts
	 */
	for (i = 0; i < PBLK_DATA_LINES; i++) {
		struct pblk_emeta *emeta;

		emeta = kmalloc(sizeof(struct pblk_emeta), GFP_KERNEL);
		if (!emeta)
			goto fail_free_emeta;

		if (lm->emeta_len[0] > KMALLOC_MAX_CACHE_SIZE) {//KMALLOC_MAX_CACHE_SIZE=0x2000
			l_mg->emeta_alloc_type = PBLK_VMALLOC_META;//使用VMALLOC分配
			emeta->buf = vmalloc(lm->emeta_len[0]);
			if (!emeta->buf) {
				kfree(emeta);
				goto fail_free_emeta;
			}

			emeta->nr_entries = lm->emeta_sec[0];
			l_mg->eline_meta[i] = emeta;
		} else {
			l_mg->emeta_alloc_type = PBLK_KMALLOC_META;

			emeta->buf = kmalloc(lm->emeta_len[0], GFP_KERNEL);
			if (!emeta->buf) {
				kfree(emeta);
				goto fail_free_emeta;
			}

			emeta->nr_entries = lm->emeta_sec[0];
			l_mg->eline_meta[i] = emeta;
		}
	}

	l_mg->vsc_list = kcalloc(l_mg->nr_lines, sizeof(__le32), GFP_KERNEL);
	if (!l_mg->vsc_list)
		goto fail_free_emeta;

	for (i = 0; i < l_mg->nr_lines; i++)
		l_mg->vsc_list[i] = cpu_to_le32(EMPTY_ENTRY);

	return 0;

fail_free_emeta:
	while (--i >= 0) {
		if (l_mg->emeta_alloc_type == PBLK_VMALLOC_META)
			vfree(l_mg->eline_meta[i]->buf);
		else
			kfree(l_mg->eline_meta[i]->buf);
		kfree(l_mg->eline_meta[i]);
	}

fail_free_smeta:
	for (i = 0; i < PBLK_DATA_LINES; i++)
		kfree(l_mg->sline_meta[i]);

	return -ENOMEM;
}

static int pblk_lines_init(struct pblk *pblk)
{
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_line *line;
	unsigned int smeta_len, emeta_len;
	long nr_bad_blks, nr_free_blks;
	int bb_distance, max_write_ppas, mod;
	int i, ret;

	pblk->min_write_pgs = geo->sec_per_pl * (geo->sec_size / PAGE_SIZE);//8
	max_write_ppas = pblk->min_write_pgs * geo->all_luns;
	pblk->max_write_pgs = (max_write_ppas < nvm_max_phys_sects(dev)) ?
				max_write_ppas : nvm_max_phys_sects(dev);//32
	pblk_set_sec_per_write(pblk, pblk->min_write_pgs);

	if (pblk->max_write_pgs > PBLK_MAX_REQ_ADDRS) {
		pr_err("pblk: cannot support device max_phys_sect\n");
		return -EINVAL;
	}

	div_u64_rem(geo->sec_per_chk, pblk->min_write_pgs, &mod);
	if (mod) {
		pr_err("pblk: bad configuration of sectors/pages\n");
		return -EINVAL;
	}

	l_mg->nr_lines = geo->nr_chks;//63

	l_mg->log_line = l_mg->data_line = NULL;
	l_mg->l_seq_nr = l_mg->d_seq_nr = 0;
	l_mg->nr_free_lines = 0;
	bitmap_zero(&l_mg->meta_bitmap, PBLK_DATA_LINES);

	lm->sec_per_line = geo->sec_per_chk * geo->all_luns;//8192
	lm->blk_per_line = geo->all_luns;//4
	lm->blk_bitmap_len = BITS_TO_LONGS(geo->all_luns) * sizeof(long);//8
	lm->sec_bitmap_len = BITS_TO_LONGS(lm->sec_per_line) * sizeof(long);//1024
	lm->lun_bitmap_len = BITS_TO_LONGS(geo->all_luns) * sizeof(long);//8
	lm->mid_thrs = lm->sec_per_line / 2;//4096
	lm->high_thrs = lm->sec_per_line / 4;//2048
	lm->meta_distance = (geo->all_luns / 2) * pblk->min_write_pgs;//
/*pr_notice("lm->blk_per_line=0x%x\n lm->blk_bitmap_len=0x%x\nlm->sec_bitmap_len=0x%x\nlm->lun_bitmap_len=0x%x\n \
	lm->mid_thrs=0x%x\nlm->high_thrs=0x%x\nlm->meta_distance=0x%x\n",
	lm->blk_per_line,lm->blk_bitmap_len,lm->sec_bitmap_len,lm->lun_bitmap_len,lm->mid_thrs,lm->high_thrs,lm->meta_distance);
*/
	/* Calculate necessary pages for smeta. See comment over struct
	 * line_smeta definition
	 */
	i = 1;
add_smeta_page:
	lm->smeta_sec = i * geo->sec_per_pl;
	lm->smeta_len = lm->smeta_sec * geo->sec_size;

	smeta_len = sizeof(struct line_smeta) + lm->lun_bitmap_len;
	if (smeta_len > lm->smeta_len) {
		i++;
		goto add_smeta_page;
	}

	/* Calculate necessary pages for emeta. See comment over struct
	 * line_emeta definition
	 */
	i = 1;
add_emeta_page:
	lm->emeta_sec[0] = i * geo->sec_per_pl;
	lm->emeta_len[0] = lm->emeta_sec[0] * geo->sec_size;

	emeta_len = calc_emeta_len(pblk);
	if (emeta_len > lm->emeta_len[0]) {
		i++;
		goto add_emeta_page;
	}

	lm->emeta_bb = geo->all_luns > i ? geo->all_luns - i : 0;

	lm->min_blk_line = 1;
	if (geo->all_luns > 1)
		lm->min_blk_line += DIV_ROUND_UP(lm->smeta_sec +
					lm->emeta_sec[0], geo->sec_per_chk);
	if (lm->min_blk_line > lm->blk_per_line) {
		pr_err("pblk: config. not supported. Min. LUN in line:%d\n",
							lm->blk_per_line);
		ret = -EINVAL;
		goto fail;
	}

	ret = pblk_lines_alloc_metadata(pblk);
	if (ret)
		goto fail;

	l_mg->bb_template = kzalloc(lm->sec_bitmap_len, GFP_KERNEL);
	if (!l_mg->bb_template) {
		ret = -ENOMEM;
		goto fail_free_meta;
	}

	l_mg->bb_aux = kzalloc(lm->sec_bitmap_len, GFP_KERNEL);
	if (!l_mg->bb_aux) {
		ret = -ENOMEM;
		goto fail_free_bb_template;
	}
	bb_distance = (geo->all_luns) * geo->sec_per_pl;//32
	for (i = 0; i < lm->sec_per_line; i += bb_distance){
		bitmap_set(l_mg->bb_template, i, geo->sec_per_pl);
	}



	INIT_LIST_HEAD(&l_mg->free_list);
	INIT_LIST_HEAD(&l_mg->corrupt_list);
	INIT_LIST_HEAD(&l_mg->bad_list);
	INIT_LIST_HEAD(&l_mg->gc_full_list);
	INIT_LIST_HEAD(&l_mg->gc_high_list);
	INIT_LIST_HEAD(&l_mg->gc_mid_list);
	INIT_LIST_HEAD(&l_mg->gc_low_list);
	INIT_LIST_HEAD(&l_mg->gc_empty_list);

	INIT_LIST_HEAD(&l_mg->emeta_list);

	l_mg->gc_lists[0] = &l_mg->gc_high_list;
	l_mg->gc_lists[1] = &l_mg->gc_mid_list;
	l_mg->gc_lists[2] = &l_mg->gc_low_list;

	spin_lock_init(&l_mg->free_lock);
	spin_lock_init(&l_mg->close_lock);
	spin_lock_init(&l_mg->gc_lock);

	pblk->lines = kcalloc(l_mg->nr_lines, sizeof(struct pblk_line),
								GFP_KERNEL);
	if (!pblk->lines) {
		ret = -ENOMEM;
		goto fail_free_bb_aux;
	}

	nr_free_blks = 0;
	for (i = 0; i < l_mg->nr_lines; i++) {
		int blk_in_line;

		line = &pblk->lines[i];

		line->pblk = pblk;
		line->id = i;
		line->type = PBLK_LINETYPE_FREE;
		line->state = PBLK_LINESTATE_FREE;
		line->gc_group = PBLK_LINEGC_NONE;
		line->vsc = &l_mg->vsc_list[i];
		spin_lock_init(&line->lock);

		ret = pblk_alloc_line_bitmaps(pblk, line);
		if (ret)
			goto fail_free_lines;

		nr_bad_blks = pblk_bb_line(pblk, line, lm->blk_per_line);
		if (nr_bad_blks < 0 || nr_bad_blks > lm->blk_per_line) {
			pblk_free_line_bitmaps(line);
			ret = -EINVAL;
			goto fail_free_lines;
		}

		blk_in_line = lm->blk_per_line - nr_bad_blks;
		if (blk_in_line < lm->min_blk_line) {
			line->state = PBLK_LINESTATE_BAD;
			list_add_tail(&line->list, &l_mg->bad_list);
			continue;
		}

		nr_free_blks += blk_in_line;
		atomic_set(&line->blk_in_line, blk_in_line);

		l_mg->nr_free_lines++;
		list_add_tail(&line->list, &l_mg->free_list);
	}

	pblk_set_provision(pblk, nr_free_blks);

	/* Cleanup per-LUN bad block lists - managed within lines on run-time */
	for (i = 0; i < geo->all_luns; i++)
		kfree(pblk->luns[i].bb_list);

	return 0;
fail_free_lines:
	while (--i >= 0)
		pblk_free_line_bitmaps(&pblk->lines[i]);
fail_free_bb_aux:
	kfree(l_mg->bb_aux);
fail_free_bb_template:
	kfree(l_mg->bb_template);
fail_free_meta:
	pblk_line_meta_free(pblk);
fail:
	for (i = 0; i < geo->all_luns; i++)
		kfree(pblk->luns[i].bb_list);

	return ret;
}

static int pblk_writer_init(struct pblk *pblk)
{
	pblk->writer_ts = kthread_create(pblk_write_ts, pblk, "pblk-writer-t");
	if (IS_ERR(pblk->writer_ts)) {
		int err = PTR_ERR(pblk->writer_ts);

		if (err != -EINTR)
			pr_err("pblk: could not allocate writer kthread (%d)\n",
					err);
		return err;
	}

	timer_setup(&pblk->wtimer, pblk_write_timer_fn, 0);
	mod_timer(&pblk->wtimer, jiffies + msecs_to_jiffies(100));

	return 0;
}

static void pblk_writer_stop(struct pblk *pblk)
{
	/* The pipeline must be stopped and the write buffer emptied before the
	 * write thread is stopped
	 */
	WARN(pblk_rb_read_count(&pblk->rwb),
			"Stopping not fully persisted write buffer\n");

	WARN(pblk_rb_sync_count(&pblk->rwb),
			"Stopping not fully synced write buffer\n");

	if (pblk->writer_ts)
		kthread_stop(pblk->writer_ts);
	del_timer(&pblk->wtimer);
}

static void pblk_free(struct pblk *pblk)
{
	pblk_luns_free(pblk);
	pblk_lines_free(pblk);
	pblk_line_meta_free(pblk);
	pblk_core_free(pblk);
	pblk_l2p_free(pblk);

	kfree(pblk);
}

static void pblk_tear_down(struct pblk *pblk)
{
	pblk_pipeline_stop(pblk);
	pblk_writer_stop(pblk);
	pblk_rb_sync_l2p(&pblk->rwb);
	pblk_rl_free(&pblk->rl);

	pr_debug("pblk: consistent tear down\n");
}

static void pblk_exit(void *private)
{
	struct pblk *pblk = private;

	down_write(&pblk_lock);
	pblk_gc_exit(pblk);
	pblk_tear_down(pblk);

#ifdef CONFIG_NVM_DEBUG
	pr_info("pblk exit: L2P CRC: %x\n", pblk_l2p_crc(pblk));
#endif

	pblk_free(pblk);
	up_write(&pblk_lock);
}

static sector_t pblk_capacity(void *private)
{
	struct pblk *pblk = private;

pr_notice("pblk->capacity=0x%lx, NR_PHY_IN_LOG=0x%x\n",pblk->capacity,NR_PHY_IN_LOG);
	return pblk->capacity * NR_PHY_IN_LOG;
}


/*
在创建tgt设备的时候调用，即在nvm_create_tgt()中調用。

nvm_tgt_dev dev: 赋值是在nvm_ioctl_dev_create()，为dev的lun_map的begin到end的lun进行设置。根据创建参数初始化
nvm_dev和nvm_tgt_dev等
gendisk tdisk: 也是由上面nvm_create_tgt()时创建并赋值的。
*/
static void *pblk_init(struct nvm_tgt_dev *dev, struct gendisk *tdisk,
		       int flags)
{
	struct nvm_geo *geo = &dev->geo;
	struct request_queue *bqueue = dev->q;
	struct request_queue *tqueue = tdisk->queue;
	struct pblk *pblk;
	int ret;

	if (dev->identity.dom & NVM_RSP_L2P) {
		pr_err("pblk: host-side L2P table not supported. (%x)\n",
							dev->identity.dom);
		return ERR_PTR(-EINVAL);
	}

	pblk = kzalloc(sizeof(struct pblk), GFP_KERNEL);
	if (!pblk)
		return ERR_PTR(-ENOMEM);

	pblk->dev = dev;
	pblk->disk = tdisk;
	pblk->state = PBLK_STATE_RUNNING;
	pblk->gc.gc_enabled = 0;

	spin_lock_init(&pblk->trans_lock);
	spin_lock_init(&pblk->lock);
//pr_notice("flags=0x%x\n",flags);
	if (flags & NVM_TARGET_FACTORY)//flags=0
		pblk_setup_uuid(pblk);

#ifdef CONFIG_NVM_DEBUG
	atomic_long_set(&pblk->inflight_writes, 0);
	atomic_long_set(&pblk->padded_writes, 0);
	atomic_long_set(&pblk->padded_wb, 0);
	atomic_long_set(&pblk->nr_flush, 0);
	atomic_long_set(&pblk->req_writes, 0);
	atomic_long_set(&pblk->sub_writes, 0);
	atomic_long_set(&pblk->sync_writes, 0);
	atomic_long_set(&pblk->inflight_reads, 0);
	atomic_long_set(&pblk->cache_reads, 0);
	atomic_long_set(&pblk->sync_reads, 0);
	atomic_long_set(&pblk->recov_writes, 0);
	atomic_long_set(&pblk->recov_writes, 0);
	atomic_long_set(&pblk->recov_gc_writes, 0);
	atomic_long_set(&pblk->recov_gc_reads, 0);
#endif

	atomic_long_set(&pblk->read_failed, 0);
	atomic_long_set(&pblk->read_empty, 0);
	atomic_long_set(&pblk->read_high_ecc, 0);
	atomic_long_set(&pblk->read_failed_gc, 0);
	atomic_long_set(&pblk->write_failed, 0);
	atomic_long_set(&pblk->erase_failed, 0);

//这里的dev->luns保存每个luns的base ppas，即ppa=0,然后设置了ppa.g.ch和ppa.g.lun。
	ret = pblk_luns_init(pblk, dev->luns);
	if (ret) {
		pr_err("pblk: could not initialize luns\n");
		goto fail;
	}

	ret = pblk_lines_init(pblk);
	if (ret) {
		pr_err("pblk: could not initialize lines\n");
		goto fail_free_luns;
	}

	ret = pblk_core_init(pblk);
	if (ret) {
		pr_err("pblk: could not initialize core\n");
		goto fail_free_line_meta;
	}

	ret = pblk_l2p_init(pblk);
	if (ret) {
		pr_err("pblk: could not initialize maps\n");
		goto fail_free_core;
	}

	ret = pblk_lines_configure(pblk, flags);
	if (ret) {
		pr_err("pblk: could not configure lines\n");
		goto fail_free_l2p;
	}

	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
pr_notice("l_mg->data_line->seq_nr=0x%x\n",l_mg->data_line->seq_nr);

	ret = pblk_writer_init(pblk);
	if (ret) {
		if (ret != -EINTR)
			pr_err("pblk: could not initialize write thread\n");
		goto fail_free_lines;
	}

	ret = pblk_gc_init(pblk);
	if (ret) {
		pr_err("pblk: could not initialize gc\n");
		goto fail_stop_writer;
	}

	/* inherit the size from the underlying device */
	blk_queue_logical_block_size(tqueue, queue_physical_block_size(bqueue));
	blk_queue_max_hw_sectors(tqueue, queue_max_hw_sectors(bqueue));

	blk_queue_write_cache(tqueue, true, false);

	tqueue->limits.discard_granularity = geo->sec_per_chk * geo->sec_size;
	tqueue->limits.discard_alignment = 0;
	blk_queue_max_discard_sectors(tqueue, UINT_MAX >> 9);
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, tqueue);

	pr_info("pblk(%s): luns:%u, lines:%d, secs:%llu, buf entries:%u\n",
			tdisk->disk_name,
			geo->all_luns, pblk->l_mg.nr_lines,
			(unsigned long long)pblk->rl.nr_secs,
			pblk->rwb.nr_entries);

	wake_up_process(pblk->writer_ts);

	/* Check if we need to start GC */
	pblk_gc_should_kick(pblk);

	return pblk;

fail_stop_writer:
	pblk_writer_stop(pblk);
fail_free_lines:
	pblk_lines_free(pblk);
fail_free_l2p:
	pblk_l2p_free(pblk);
fail_free_core:
	pblk_core_free(pblk);
fail_free_line_meta:
	pblk_line_meta_free(pblk);
fail_free_luns:
	pblk_luns_free(pblk);
fail:
	kfree(pblk);
	return ERR_PTR(ret);
}

/* physical block device target */
static struct nvm_tgt_type tt_pblk = {
	.name		= "pblk",
	.version	= {1, 0, 0},

	.make_rq	= pblk_make_rq,
	.capacity	= pblk_capacity,

	.init		= pblk_init,
	.exit		= pblk_exit,

	.sysfs_init	= pblk_sysfs_init,
	.sysfs_exit	= pblk_sysfs_exit,
	.owner		= THIS_MODULE,
};

static int __init pblk_module_init(void)
{
	int ret;

	pblk_bio_set = bioset_create(BIO_POOL_SIZE, 0, 0);
//只在pblk_submit_read()时用到过一次，int_bio = bio_clone_fast(bio, GFP_KERNEL, pblk_bio_set);
	if (!pblk_bio_set)
		return -ENOMEM;
	ret = nvm_register_tgt_type(&tt_pblk);
	//将pblk类型注册到&nvm_tgt_types链表上，等到上层nvm lnvm create -d nvme0n1 -b 0 -e 3 -n mypblk -t pblk
	//时通过调用nvm_ioctl_dev_create()才会对pblk的target进行创建，从而调用pblk_init,并将pblk_make_rq注册到
	//tdisk->queue的make_request上面。
	if (ret)
		bioset_free(pblk_bio_set);
	return ret;
}

static void pblk_module_exit(void)
{
	bioset_free(pblk_bio_set);
	nvm_unregister_tgt_type(&tt_pblk);
}

module_init(pblk_module_init);
module_exit(pblk_module_exit);
MODULE_AUTHOR("Javier Gonzalez <javier@cnexlabs.com>");
MODULE_AUTHOR("Matias Bjorling <matias@cnexlabs.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Physical Block-Device for Open-Channel SSDs");
