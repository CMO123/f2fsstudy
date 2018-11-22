#ifndef TGT_H_
#define TGT_H_

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/crc32.h>
#include <linux/uuid.h>
#include <linux/delay.h>

#include <linux/lightnvm.h>

#include "f2fs.h"

#define tgt_dma_meta_size (sizeof(unsigned long) * 128)

static struct lightpblk *light_pblk = NULL;

struct tgt_addr_format {
	u64	ch_mask;
	u64	lun_mask;
	u64	pln_mask;
	u64	blk_mask;
	u64	pg_mask;
	u64	sec_mask;
	u8	ch_offset;
	u8	lun_offset;
	u8	pln_offset;
	u8	blk_offset;
	u8	pg_offset;
	u8	sec_offset;
};

struct lightpblk {
	struct nvm_tgt_dev *tgt_dev;
	unsigned char *trans_map;
	spinlock_t trans_lock;
	int min_write_pgs;
	unsigned int paralun;
	int nrpl;
	int nrlun;
	int sec_per_pg;
	struct tgt_addr_format ppaf;
	int ppaf_bitsize;
	
};

//int tgt_submit_page_read(struct f2fs_sb_info *sbi, struct page* page, block_t paddr);

//int tgt_submit_page_erase(struct f2fs_sb_info *sbi, block_t paddr, uint32_t nr_blks);
//int tgt_submit_page_write(struct f2fs_sb_info *sbi, struct page* page, 
	//						block_t pblkaddr, uint8_t sync);


static void lightpblk_print_geo(struct nvm_geo *geo){
	pr_notice("-----lightpblk_print_geo---------\n");
	pr_notice("chnls=0x%x,all_luns=0x%x,nrluns=0x%x,nr_chks=0x%x\n",geo->nr_chnls,geo->all_luns,geo->nr_luns,geo->nr_chks);
	pr_notice("secsize=0x%x,oobsize=0x%x,mccap=0x%x\n",geo->sec_size,geo->oob_size,geo->mccap);
	pr_notice("secPerchk=0x%x,secPerLun=0x%x\n",geo->sec_per_chk,geo->sec_per_lun);
	pr_notice("wsmin=0x%x,wsopt=0x%x,wsseq=0x%x,wsperchk=0x%x\n",geo->ws_min,geo->ws_opt,geo->ws_seq,geo->ws_per_chk);
	pr_notice("max_rq_size=0x%x,op=0x%x\n",geo->max_rq_size,geo->op);
	pr_notice("(choff=0x%x,chlen=0x%x)\n(lunoff=0x%x,lunlen=0x%x)\n(plnoff=0x%x,plnlen=0x%x)\n(blkoff=0x%x,blklen=0x%x)\n(pgoff=0x%x,pglen=0x%x)\n(secoff=0x%x,seclen=0x%x)\n",
		geo->ppaf.ch_offset,geo->ppaf.ch_len,
		geo->ppaf.lun_offset,geo->ppaf.lun_len,
		geo->ppaf.pln_offset,geo->ppaf.pln_len,
		geo->ppaf.blk_offset,geo->ppaf.blk_len,
		geo->ppaf.pg_offset,geo->ppaf.pg_len,
		geo->ppaf.sect_offset,geo->ppaf.sect_len);
	pr_notice("planeMode=0x%x,nr_planes=0x%x,secPerPg=0x%x,secPerPl=0x%x\n",geo->plane_mode,geo->nr_planes,geo->sec_per_pg,geo->sec_per_pl);
	
}
static int tgt_set_ppaf(struct lightpblk* alightpblk)
{//用于组织自身逻辑设备的地址,类型liblightnvm
	struct nvm_tgt_dev *dev = alightpblk->tgt_dev;
	struct nvm_geo *geo = &dev->geo;
	struct nvm_addr_format ppaf = geo->ppaf;
	int power_len;

	power_len = get_count_order(geo->nr_chnls);
	if(1 << power_len != geo->nr_chnls){
		pr_err("tgt_pblk: supports only power-of-two channel config.\n");
		return -EINVAL;
	}
	
	power_len = get_count_order(geo->nr_luns);
	if (1 << power_len != geo->nr_luns) {
		pr_err("tgt_pblk: supports only power-of-two LUN config.\n");
		return -EINVAL;
	}
	
	
	alightpblk->ppaf.sec_offset = ppaf.sect_offset;
	alightpblk->ppaf.pln_offset = ppaf.pln_offset;
	alightpblk->ppaf.ch_offset = ppaf.ch_offset;
	alightpblk->ppaf.lun_offset = ppaf.lun_offset;
	alightpblk->ppaf.pg_offset = ppaf.pg_offset;
	alightpblk->ppaf.blk_offset = ppaf.blk_offset;

	
	alightpblk->ppaf.sec_mask = ((1ULL << ppaf.sect_len) - 1) << ppaf.sect_offset;
	alightpblk->ppaf.pln_mask = ((1ULL << ppaf.pln_len) - 1) <<	alightpblk->ppaf.pln_offset;
	alightpblk->ppaf.ch_mask = ((1ULL << ppaf.ch_len) - 1) <<
							alightpblk->ppaf.ch_offset;
	alightpblk->ppaf.lun_mask = ((1ULL << ppaf.lun_len) - 1) <<
							alightpblk->ppaf.lun_offset;
	alightpblk->ppaf.pg_mask = ((1ULL << ppaf.pg_len) - 1) <<
							alightpblk->ppaf.pg_offset;
	alightpblk->ppaf.blk_mask = ((1ULL << ppaf.blk_len) - 1) <<
							alightpblk->ppaf.blk_offset;

	alightpblk->ppaf_bitsize = alightpblk->ppaf.blk_offset + ppaf.blk_len;

	/*pr_notice("ppaf.ch_len = %d, ppaf.lun_len = %d, ppaf.sect_len = %d, ppaf.pln_len = %d, ppaf.pg_len = %d, ppaf.blk_len = %d \n",
		ppaf.ch_len, ppaf.lun_len, ppaf.sect_len, ppaf.pln_len, ppaf.pg_len, ppaf.blk_len);
	pr_notice("ppaf.ch_offset = %d, ppaf.lun_offset = %d, ppaf.pln_offset = %d, ppaf.blk_offset = %d, ppaf.pg_offset = %d, ppaf.sect_offset = %d \n",
		ppaf.ch_offset, ppaf.lun_offset, ppaf.pln_offset, ppaf.blk_offset, ppaf.pg_offset, ppaf.sect_offset);
	*/
	/*
		ppaf.ch_len = 3, ppaf.lun_len = 0, ppaf.sect_len = 0, ppaf.pln_len = 0, ppaf.pg_len = 9, ppaf.blk_len = 10 
		[  675.646464] ppaf.ch_offset = 19, ppaf.lun_offset = 19, ppaf.pln_offset = 0, ppaf.blk_offset = 9, ppaf.pg_offset = 0, ppaf.sect_offset = 0 

	*/	
	/*
	pr_notice("alightpblk->ppaf.sec_mask = 0x%llx,alightpblk->ppaf.pln_mask = 0x%llx,alightpblk->ppaf.ch_mask = 0x%llx, \
			alightpblk->ppaf.lun_mask = 0x%llx,alightpblk->ppaf.pg_mask = 0x%llx, \
			alightpblk->ppaf.blk_mask = 0x%llx,alightpblk->ppaf_bitsize = %d \n",
			alightpblk->ppaf.sec_mask,alightpblk->ppaf.pln_mask,alightpblk->ppaf.ch_mask,
			alightpblk->ppaf.lun_mask,alightpblk->ppaf.pg_mask,
			alightpblk->ppaf.blk_mask,alightpblk->ppaf_bitsize);
	*/
	/*
	alightpblk->ppaf.sec_mask = 0x0,alightpblk->ppaf.pln_mask = 0x0,alightpblk->ppaf.ch_mask = 0x7,
	alightpblk->ppaf.lun_mask = 0x0,alightpblk->ppaf.pg_mask = 0xff8, 
	alightpblk->ppaf.blk_mask = 0x3ff000,
	alightpblk->ppaf_bitsize = 22
	*/

	/*		
	pr_notice("alightpblk->ppaf.sec_offset = 0x%x\n,alightpblk->ppaf.pln_offset= 0x%x\n, \
	alightpblk->ppaf.ch_offset= 0x%x\n, alightpblk->ppaf.lun_offset= 0x%x\n,alightpblk->ppaf.pg_offset= 0x%x\n,	\
	alightpblk->ppaf.blk_offset= 0x%x\n",alightpblk->ppaf.sec_offset,alightpblk->ppaf.pln_offset,
	alightpblk->ppaf.ch_offset, alightpblk->ppaf.lun_offset,alightpblk->ppaf.pg_offset,	alightpblk->ppaf.blk_offset);
	*/
	/*
	[ 5354.208847] alightpblk->ppaf.sec_offset = 0x0
	,alightpblk->ppaf.pln_offset= 0x0
	,	alightpblk->ppaf.ch_offset= 0x0
	, alightpblk->ppaf.lun_offset= 0x3
	,alightpblk->ppaf.pg_offset= 0x3
	,		alightpblk->ppaf.blk_offset= 0xc

	*/
	
	return 0;
}

static void* lightpblk_init(struct nvm_tgt_dev *dev, struct gendisk **ptdisk, struct nvm_ioctl_create *create)
{
	int ret = 0;	
	struct lightpblk *alightpblk;
	struct nvm_geo* geo;

pr_notice("Enter lightpblk_init()\n");

	alightpblk= kzalloc(sizeof(struct lightpblk), GFP_KERNEL);
	if (!alightpblk)
		return ERR_PTR(-ENOMEM);
	alightpblk->tgt_dev = dev;
	geo = &dev->geo;
	light_pblk = alightpblk;
	alightpblk->min_write_pgs = geo->ws_opt;
	alightpblk->paralun = geo->nr_chnls * geo->nr_luns;
	alightpblk->nrpl = geo->nr_planes;
	alightpblk->nrlun = geo->nr_luns;
	alightpblk->sec_per_pg = geo->sec_per_pg;
	spin_lock_init(&alightpblk->trans_lock);
	lightpblk_print_geo(geo);
	ret= tgt_set_ppaf(alightpblk);
	
	return alightpblk;	
}
static void lightpblk_exit(void *private)
{
	struct lightpblk* alightpblk = private;
	kfree(alightpblk);
	light_pblk = NULL;	
pr_notice("Exit lightpblk_exit()\n");

}


static struct nvm_tgt_type tt_tgt = {
	.name = "lightpblk",
	.version = {1, 0, 0},

	.init = lightpblk_init,
	.exit = lightpblk_exit,
	.owner = THIS_MODULE,
};


//创建tgt实例
//struct lightpblk* lightpblk_fs_create(struct super_block* sb, char* name)
static struct lightpblk* lightpblk_fs_create(struct super_block* sb, char* name)
{
	struct nvm_ioctl_create create;
	struct block_device *tgt_bdev = sb->s_bdev;
	char dev_name[DISK_NAME_LEN]="";
	int err;

//1. 组件create参数，创建tgt_dev
	//pr_notice(" tgt_bdev->bd_disk->disk_name = %s\n", tgt_bdev->bd_disk->disk_name);//nvme0n1即可
	
	strcat(dev_name, tgt_bdev->bd_disk->disk_name);
	
	strlcpy(create.dev,dev_name,DISK_NAME_LEN);//即nvme0n1
	strlcpy(create.tgttype, tt_tgt.name, NVM_TTYPE_NAME_MAX);
	strlcpy(create.tgtname, name, DISK_NAME_LEN);
	create.flags = 0;	

	//由于ovp在fs中，故使用simple
	create.conf.type = NVM_CONFIG_TYPE_SIMPLE;
	create.conf.s.lun_begin = -1;
	create.conf.s.lun_end = -1;
	
	create.dev[DISK_NAME_LEN - 1] = '\0';
	create.tgttype[NVM_TTYPE_NAME_MAX - 1] = '\0';
	create.tgtname[DISK_NAME_LEN - 1] = '\0';

	pr_notice("create.dev = %s\n", create.dev);
	pr_notice("create.tgttype = %s\n", create.tgttype);
	pr_notice("create.tgtname = %s\n", create.tgtname);


	err = __nvm_configure_create(&create);
	if(err)
	{
		pr_notice("__nvm_configure_create() error\n");
		return NULL;
	}

	return light_pblk;
}

//===================================================================================================
static inline void print_ppa(struct ppa_addr *p, char *msg, int error)
{
	if (p->c.is_cached) {
		pr_err("ppa: (%s: %x) cache line: %llu\n",
				msg, error, (u64)p->c.line);
	} else {
		pr_err("ppa: (%s: %x):ch:%d,lun:%d,blk:%d,pg:%d,pl:%d,sec:%d\n",
			msg, error,
			p->g.ch, p->g.lun, p->g.blk,
			p->g.pg, p->g.pl, p->g.sec);
	}
}


static inline struct ppa_addr addr_ppa32_to_ppa64(struct f2fs_sb_info *sbi, u32 ppa32)
{
	struct lightpblk* lightpblk = sbi->s_lightpblk;

	struct ppa_addr ppa64;
	ppa64.ppa = 0;

	if (ppa32 == -1) {
		ppa64.ppa = ADDR_EMPTY;
	} else if (ppa32 & (1U << 31)) {
		ppa64.c.line = ppa32 & ((~0U) >> 1);
		ppa64.c.is_cached = 1;
	} else {
		
		ppa64.g.blk = (ppa32 & lightpblk->ppaf.blk_mask) >>
							lightpblk->ppaf.blk_offset;
			
		ppa64.g.pg = (ppa32 & lightpblk->ppaf.pg_mask) >>
							lightpblk->ppaf.pg_offset;
		
		ppa64.g.lun = (ppa32 & lightpblk->ppaf.lun_mask) >>
							lightpblk->ppaf.lun_offset;
		
		ppa64.g.ch = (ppa32 & lightpblk->ppaf.ch_mask) >>
							lightpblk->ppaf.ch_offset;
		
		ppa64.g.pl = (ppa32 & lightpblk->ppaf.pln_mask) >>
							lightpblk->ppaf.pln_offset;
		
		ppa64.g.sec = (ppa32 & lightpblk->ppaf.sec_mask) >>
							lightpblk->ppaf.sec_offset;
	}

	/*pr_notice("ppa64.g.blk = 0x%x ,ppa64.g.pg= 0x%x ,ppa64.g.lun = 0x%x ,ppa64.g.ch = 0x%x ,ppa64.g.pl = 0x%x ,ppa64.g.sec = 0x%x \n",ppa64.g.blk,
					ppa64.g.pg,ppa64.g.lun,ppa64.g.ch,ppa64.g.pl,ppa64.g.sec);
	pr_notice("ppa64.ppa = 0x%llx\n",ppa64.ppa);
	*/
	/*
	[ 5354.208884] ppa64.g.blk = 0x0 ,ppa64.g.pg= 0x40 ,ppa64.g.lun = 0x0 ,ppa64.g.ch = 0x0 ,ppa64.g.pl = 0x0 ,ppa64.g.sec = 0x0 
	[ 5354.208885] ppa64.ppa = 0x400000 
	*/

	return ppa64;
}

static struct ppa_addr addr_to_gen_ppa(struct f2fs_sb_info* sbi, block_t paddr){
		struct lightpblk* lightpblk = sbi->s_lightpblk;
		struct ppa_addr ppa;
		//u32 paddr32 = le32_to_cpu(paddr);
		u64 paddr64 = (u64)paddr;
	
		ppa.ppa = 0;
		
		ppa.g.blk =(paddr64 & lightpblk->ppaf.blk_mask) >> lightpblk->ppaf.blk_offset;
		ppa.g.pg = (paddr64 & lightpblk->ppaf.pg_mask) >> lightpblk->ppaf.pg_offset;
		ppa.g.lun = (paddr64 & lightpblk->ppaf.lun_mask) >> lightpblk->ppaf.lun_offset;
		ppa.g.ch = (paddr64 & lightpblk->ppaf.ch_mask) >> lightpblk->ppaf.ch_offset;
		ppa.g.pl = (paddr64 & lightpblk->ppaf.pln_mask) >> lightpblk->ppaf.pln_offset;
		ppa.g.sec = (paddr64 & lightpblk->ppaf.sec_mask) >> lightpblk->ppaf.sec_offset;
		pr_notice("ppa.g.blk = 0x%x ,ppa.g.pg= 0x%x ,ppa.g.lun = 0x%x ,ppa.g.ch = 0x%x ,ppa.g.pl = 0x%x ,ppa.g.sec = 0x%x \n",ppa.g.blk,
					ppa.g.pg,ppa.g.lun,ppa.g.ch,ppa.g.pl,ppa.g.sec);
		pr_notice("ppa.ppa = 0x%llx\n",ppa.ppa);
		return ppa;

}

static inline int tgt_boundary_ppa_checks(struct nvm_tgt_dev *tgt_dev, struct ppa_addr* ppas, int nr_ppas)
{
	struct nvm_geo *geo = &tgt_dev->geo;
	struct ppa_addr *ppa;
	int i;
	//pr_notice("geo->nr_chnls = 0x%x\n", geo->nr_chnls);
	
		for (i = 0; i < nr_ppas; i++) {
			ppa = &ppas[i];
			//pr_notice("ppa->g.ch = 0x%llx\n",ppa->g.ch);
			
			 if (!ppa->c.is_cached &&
					ppa->g.ch < geo->nr_chnls &&
					ppa->g.lun < geo->nr_luns &&
					ppa->g.pl < geo->nr_planes &&
					ppa->g.blk < geo->nr_chks &&
					ppa->g.pg < geo->ws_per_chk &&
					ppa->g.sec < geo->sec_per_pg)
				continue;
	
			print_ppa(ppa, "boundary", i);
			
			return 1;
		}
	
		return 0;


}
/*
static inline struct ppa_addr tgt_ppa32_to_ppa64(struct lightpblk* alightpblk, u32 ppa32)
{
	struct ppa_addr ppa64;

	ppa64.ppa = 0;

	if (ppa32 == -1) {
		ppa64.ppa = ADDR_EMPTY;
	} else {
		ppa64.g.blk = (ppa32 & alightpblk->ppaf.blk_mask) >>
							alightpblk->ppaf.blk_offset;
		ppa64.g.pg = (ppa32 & alightpblk->ppaf.pg_mask) >>
							alightpblk->ppaf.pg_offset;
		ppa64.g.lun = (ppa32 & alightpblk->ppaf.lun_mask) >>
							alightpblk->ppaf.lun_offset;
		ppa64.g.ch = (ppa32 & alightpblk->ppaf.ch_mask) >>
							alightpblk->ppaf.ch_offset;
		ppa64.g.pl = (ppa32 & alightpblk->ppaf.pln_mask) >>
							alightpblk->ppaf.pln_offset;
		ppa64.g.sec = (ppa32 & alightpblk->ppaf.sec_mask) >>
							alightpblk->ppaf.sec_offset;
	}

	return ppa64;
}

static inline u32 tgt_ppa64_to_ppa32(struct lightpblk *alightpblk, struct ppa_addr ppa64)
{
	u32 ppa32 = 0;

	if (ppa64.ppa == ADDR_EMPTY) {
		ppa32 = ~0U;
	} else if (ppa64.c.is_cached) {
		ppa32 |= ppa64.c.line;
		ppa32 |= 1U << 31;
	} else {
		ppa32 |= ppa64.g.blk << alightpblk->ppaf.blk_offset;
		ppa32 |= ppa64.g.pg << alightpblk->ppaf.pg_offset;
		ppa32 |= ppa64.g.lun << alightpblk->ppaf.lun_offset;
		ppa32 |= ppa64.g.ch << alightpblk->ppaf.ch_offset;
		ppa32 |= ppa64.g.pl << alightpblk->ppaf.pln_offset;
		ppa32 |= ppa64.g.sec << alightpblk->ppaf.sec_offset;
	}

	return ppa32;
}
*/


static void tgt_end_io_write(struct nvm_rq* rqd)
{
	struct bio* bio = rqd->bio;
	bio_put(bio);

	nvm_dev_dma_free(rqd->dev->parent, rqd->meta_list, rqd->dma_meta_list);
	kfree(rqd);
	
	pr_notice("tgt_end_io_write()\n");
}
static void tgt_end_io_erase(struct nvm_rq* rqd)
{
	//return NULL;
	
	struct bio* bio = rqd->bio;
	pr_notice("=====================Enter tgt_end_io_erase()\n");
	pr_notice("rqd->ppa_addr.ppa = 0x%llx\n", rqd->ppa_addr.ppa);
	kfree(rqd);
}


static void f2fs_write_end_io2(struct bio *bio)
{
	bio_put(bio);
}

static void tgt_end_io_read(struct nvm_rq* rqd)
{
	
	//pr_notice("================== tgt_end_io_read()\n");
	nvm_dev_dma_free(rqd->dev->parent, rqd->meta_list, rqd->dma_meta_list);
	kfree(rqd);
	
}
static void bio_map_addr_endio(struct bio* bio){
	bio_put(bio);
}
//==========================================================================
static int tgt_setup_r_rq(struct f2fs_sb_info *sbi, struct nvm_rq* rqd, int nr_secs, nvm_end_io_fn(*end_io))
{
	struct nvm_tgt_dev* tgt_dev = sbi->s_lightpblk->tgt_dev;
	struct nvm_geo* geo = &tgt_dev->geo;

	/*设置rqd*/
	rqd->dev = tgt_dev;
	rqd->opcode = NVM_OP_PREAD;
	rqd->nr_ppas = nr_secs;
	rqd->private = sbi;
	rqd->end_io = end_io;
	rqd->meta_list = nvm_dev_dma_alloc(tgt_dev->parent, GFP_KERNEL,
							&rqd->dma_meta_list);	
	if (!rqd->meta_list) {
			pr_err("pblk: not able to allocate ppa list\n");
			return -EFAULT;
	}
	if (nr_secs > 1) {
		rqd->ppa_list = rqd->meta_list + tgt_dma_meta_size;
		rqd->dma_ppa_list = rqd->dma_meta_list + tgt_dma_meta_size;
		rqd->flags = NVM_IO_SUSPEND | NVM_IO_SCRAMBLE_ENABLE;	
		rqd->flags |= geo->plane_mode >> 1;//sequential读
	}else{
		rqd->flags = NVM_IO_SUSPEND | NVM_IO_SCRAMBLE_ENABLE;	
		rqd->flags |= geo->plane_mode >> 1;//sequential读
	}
	
	return 0;
}
static int tgt_setup_w_rq(struct f2fs_sb_info* sbi, struct nvm_rq* rqd, int nr_secs, nvm_end_io_fn(*end_io)){
	struct nvm_tgt_dev* tgt_dev = sbi->s_lightpblk->tgt_dev;
	struct nvm_geo* geo = &tgt_dev->geo;
	
	
	rqd->opcode = NVM_OP_PWRITE;
	rqd->nr_ppas = nr_secs;
	rqd->flags = (geo->plane_mode >> 1) | NVM_IO_SCRAMBLE_ENABLE;
	rqd->private = sbi;
	rqd->end_io = end_io;

	rqd->meta_list = nvm_dev_dma_alloc(tgt_dev->parent, GFP_KERNEL,
							&rqd->dma_meta_list);
	if (!rqd->meta_list)
		return -ENOMEM;

	rqd->ppa_list = rqd->meta_list + tgt_dma_meta_size;
	rqd->dma_ppa_list = rqd->dma_meta_list + tgt_dma_meta_size;

	rqd->ppa_status = rqd->error = 0;

	return 0;
}


static void tgt_setup_e_rq(struct f2fs_sb_info* sbi, struct nvm_rq* rqd, struct ppa_addr ppa){
	struct nvm_tgt_dev* tgt_dev = sbi->s_lightpblk->tgt_dev;
	struct nvm_geo* geo = &tgt_dev->geo;
	
	rqd->opcode = NVM_OP_ERASE;
	rqd->ppa_addr = ppa;
	rqd->nr_ppas = 1;
	rqd->flags = geo->plane_mode >> 1;
	//rqd->flags = 0x1;
	rqd->bio = NULL;
	rqd->ppa_list = rqd->meta_list = NULL;
	rqd->private = sbi;

	rqd->ppa_status = rqd->error = 0;

}
//==========================================================================

static int tgt_submit_page_write_async(struct f2fs_sb_info *sbi, struct page* page, block_t pblkaddr)
{
	//struct block_device* bdev = sbi->sb->s_bdev;	
	struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
	struct nvm_geo* geo = &dev->geo;
	struct bio* bio = NULL;
	unsigned int nr_ppas = 1;
	int i;
	struct nvm_rq* rqd;
	int ret;


	/*创建bio*/
	bio = bio_alloc(GFP_KERNEL, nr_ppas);
	//bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector = 0;
	bio->bi_iter.bi_sector = 0; /* internal bio */
	bio->bi_end_io = bio_map_addr_endio;
	//bio->bi_private = sbi;
	bio_set_op_attrs(bio, REQ_OP_WRITE, 0);	

	if(bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE)
	{
		pr_err("tgt_pblk: Error occur while calling tgt_submit_write");
		bio_put(bio);
		return -EFAULT;
	}

retry:	
	/*创建rqd*/
	rqd = kzalloc(sizeof(struct nvm_rq),GFP_KERNEL);
	if (!rqd) {
		cond_resched();
		goto retry;
	}
	rqd->bio = bio;
	ret = tgt_setup_w_rq(sbi,rqd, nr_ppas, tgt_end_io_write);
	
	//rqd->ppa_list[0] = addr_to_gen_ppa(sbi, pblkaddr);
	rqd->ppa_addr = addr_ppa32_to_ppa64(sbi, pblkaddr);
	
	ret = nvm_submit_io(dev, rqd);
	if(ret){
		pr_err("tgt_pblk: nvm_submit_write I/O submission failed: %d\n", ret);
		bio_put(bio);
	}
	return ret;
}

static int tgt_submit_page_write_sync(struct f2fs_sb_info *sbi, struct page* page, block_t pblkaddr)
{
	struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
	struct nvm_geo* geo = &dev->geo;
	struct bio* bio = NULL;
	unsigned int nr_ppas = 1;
	int i;
	struct nvm_rq rqd;
	int ret;

	memset(&rqd, 0, sizeof(struct nvm_rq));

	/*创建bio*/
	bio = bio_alloc(GFP_KERNEL, nr_ppas);
	if(!bio)
		return -ENOMEM;	
	bio->bi_iter.bi_sector = 0; /* internal bio */
	bio->bi_end_io = bio_map_addr_endio;
	//bio->bi_private = sbi;
	bio_set_op_attrs(bio, REQ_OP_WRITE, 0);	

	if(bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE)
	{
		pr_err("tgt_pblk: Error occur while calling tgt_submit_write");
		bio_put(bio);
		return -EFAULT;
	}

retry:	
	/*创建rqd*/
	rqd.bio = bio;	
	ret = tgt_setup_w_rq(sbi, &rqd, nr_ppas,NULL);
	rqd.ppa_addr = addr_ppa32_to_ppa64(sbi, pblkaddr);
	
	ret = nvm_submit_io_sync(dev, &rqd);
	if(ret){
		pr_err("tgt_pblk: nvm_submit_write I/O submission failed: %d\n", ret);
		bio_put(bio);
		goto free_rqd_dma;
	}
	//可以添加特定的end_io()
free_rqd_dma:
	nvm_dev_dma_free(dev->parent, rqd.meta_list, rqd.dma_meta_list);
	return ret;
}

//tgt的write函数
static int tgt_submit_page_write(struct f2fs_sb_info *sbi, struct page* page, block_t pblkaddr, int sync){
	if(sync){
		return tgt_submit_page_write_sync(sbi, page, pblkaddr);
	}else{
		return tgt_submit_page_write_async(sbi, page, pblkaddr);
	}

}


static int tgt_submit_page_erase_async(struct f2fs_sb_info *sbi, struct ppa_addr paddr){
		struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
		struct nvm_rq* rqd;
		int err;

		rqd = kzalloc(sizeof(struct nvm_rq),GFP_KERNEL);

		tgt_setup_e_rq(sbi, rqd, paddr);		
		rqd->end_io = tgt_end_io_erase;
		rqd->private = rqd;//传入end_io
		
		pr_notice("tgt_submit_erase(): ppa = 0x%llx\n", paddr.ppa);
	
		if(tgt_boundary_ppa_checks(dev, &paddr, 1)){
			pr_notice("tgt: tgt_boundary_ppa_checks_error()\n");
		}		
		
		err =  nvm_submit_io(dev, rqd);
		if(err){
			pr_notice("tgt: tgt_submit_page_erase_async() error\n");
		}
		return err;
		
}

static int tgt_submit_page_erase_sync(struct f2fs_sb_info *sbi, struct ppa_addr paddr){
	struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
	struct nvm_rq rqd;
	int ret = 0;

	memset(&rqd, 0, sizeof(struct nvm_rq));
	tgt_setup_e_rq(sbi, &rqd, paddr);

	
	if(tgt_boundary_ppa_checks(dev, &paddr, 1)){
		pr_notice("tgt: tgt_boundary_ppa_checks_error()\n");
	}
	
	pr_notice("tgt_submit_erase(): ppa = 0x%llx\n", paddr.ppa);
		
	ret = nvm_submit_io_sync(dev, &rqd);
	if(ret){
		pr_notice("tgt: tgt_submit_page_erase_sync() error\n");
		rqd.error = ret;
	}
	rqd.private = sbi;
	//可以添加特定的rqd的__pblk_end_io_erase();
	return ret;
}


//erase的提交函数
static int tgt_submit_addr_erase_async(struct f2fs_sb_info* sbi, block_t paddr, uint32_t nr_blks){
	struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
	int erase_num = 0;
	int pg_per_sec = sbi->segs_per_sec * (1 << sbi->log_blocks_per_seg);
	int i;
	int ret;

	if(nr_blks == pg_per_sec){//每次擦除一个section，512对应设备一个line中的4个blk
		erase_num = 1;
		pr_notice("erase 1 blk\n");
	}
	
	//有一个大问题，就是数据的组织，segment的粒度，烦，先暂时按一个ppa一个ppa地擦除
	for(i = 0; i < erase_num; i++){
		struct ppa_addr  ppa = addr_ppa32_to_ppa64(sbi, paddr+i);

		ppa.g.pg = 0;
		ppa.g.pl = 0;
		ppa.g.sec = 0;
		
		if(tgt_boundary_ppa_checks(dev, &ppa, 1)){
				pr_notice("tgt_boundary_ppa_checks_error()\n");
		}
		ret = tgt_submit_page_erase_async(sbi, ppa);
		if(ret){
			pr_notice("tgt: tgt_submit_addr_erase_async() error\n");
		}
	}
	
	return ret;
}


static int tgt_submit_page_read_sync(struct f2fs_sb_info *sbi, struct page* page, block_t paddr)
{
	struct nvm_tgt_dev *dev = sbi->s_lightpblk->tgt_dev;
	struct block_device* bdev = sbi->sb->s_bdev;
	struct nvm_geo* geo = &dev->geo;
	struct nvm_rq rqd;
	struct bio* bio = NULL;
	unsigned int nr_secs = 1;
	int ret = 0;

	memset(&rqd, 0, sizeof(struct nvm_rq));
	
	/*创建一个bio*/
	bio = bio_alloc(GFP_KERNEL, 1);	
	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector = 0;
	bio->bi_end_io = bio_map_addr_endio;
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	if (bio_add_page (bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		pr_err("Error occur while calling risa_readpage");
		bio_put (bio);
		return -EFAULT;
	}

	rqd.bio = bio;
	ret = tgt_setup_r_rq(sbi, &rqd, 1, NULL);
	rqd.ppa_addr = addr_ppa32_to_ppa64(sbi, paddr);

	
	ret = nvm_submit_io_sync(dev, &rqd);
	//pr_notice("rqd.bio.bi_status = %d\n",rqd.bio->bi_status);
	if(ret){
		pr_err("tgt_pblk: emeta I/O submission failed: %d\n", ret);
		bio_put(bio);
		goto free_rqd_dma;
		
	}
free_rqd_dma:
	nvm_dev_dma_free(dev->parent, rqd.meta_list, rqd.dma_meta_list);
	return ret;
}



static int tgt_submit_page_read_async(struct f2fs_sb_info *sbi, struct page* page, block_t paddr)
{
	struct nvm_tgt_dev *dev = sbi->s_lightpblk->tgt_dev;
	struct block_device* bdev = sbi->sb->s_bdev;
	struct nvm_geo* geo = &dev->geo;
	struct nvm_rq *rqd;
	struct bio* bio = NULL;
	unsigned int nr_secs = 1;
	int ret =0, j = 0;
	

//pr_notice("read paddr = %d\n", paddr);
	
	/*创建一个bio*/
	bio = bio_alloc(GFP_KERNEL, 1);
	
	bio_set_dev(bio, bdev);
	//bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(paddr);//根据blk_addr计算sector地址
	bio->bi_iter.bi_sector = 0;
	bio->bi_end_io = bio_map_addr_endio;
	bio->bi_private = sbi;	
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	if (bio_add_page (bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		pr_err("Error occur while calling risa_readpage");
		bio_put (bio);
		return -EFAULT;
	}


	/*设置rqd*/
	rqd = kzalloc(sizeof(struct nvm_rq), GFP_KERNEL);
	rqd->bio = bio;
	ret = tgt_setup_r_rq(sbi, rqd, 1, tgt_end_io_read);

	if (nr_secs > 1) {
		rqd->ppa_list[j++] = addr_ppa32_to_ppa64(sbi, paddr);
	}else{
		rqd->ppa_addr = addr_ppa32_to_ppa64(sbi, paddr);
	}
	
	ret =  nvm_submit_io(dev, rqd);
	if(ret){
		pr_err("tgt: tgt_submit_page_read_async() error, ret = %d\n", ret);
		bio_put(bio);		
	}
	return ret;
}
/*
int tgt_submit_io_sync(struct f2fs_sb_info* sbi, struct page* page, block_t paddr, int dir){
	struct nvm_tgt_dev* dev = sbi->s_lightpblk->tgt_dev;
	
	struct nvm_geo* geo = &dev->geo;
	void* ppa_list, *meta_list;
	struct bio* bio;
	struct nvm_rq rqd;
	dma_addr_t dma_ppa_list, dma_meta_list;
	int rq_ppas, rq_len;
	int cmd_op, bio_op;
	int ret;

	if(dir == WRITE){
		bio_op = REQ_OP_WRITE;
		cmd_op = NVM_OP_PWRITE;
	}else if(dir == READ){
		bio_op = REQ_OP_READ;
		cmd_op = NVM_OP_PREAD;
	}else 
		return -EINVAL;

	meta_list = nvm_dev_dma_alloc(dev->parent, GFP_KERNEL, &dma_meta_list);
	if(!meta_list)
		return -ENOMEM;

	ppa_list = meta_list + tgt_dma_meta_size;
	dma_ppa_list = dma_meta_list + tgt_dma_meta_size;
	//创建一条命令
	memset(&rqd, 0, sizeof(struct nvm_rq));
	rq_ppas = 1;
	rq_len = rq_ppas * geo->sec_size;

	//分配bio
	bio = bio_alloc(GFP_KERNEL, rq_ppas);
	if(!bio)
		return -ENOMEM;	
	if(bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE){
		pr_err("tgt_pblk: Error occur while calling tgt_submit_write");
		bio_put(bio);
		return -EFAULT;
	}
	bio->bi_end_io = bio_map_addr_endio;

	bio->bi_iter.bi_sector = 0;//内部bio
	bio_set_op_attrs(bio, bio_op, 0);

	//rqd组合bio
	rqd.bio = bio;
	rqd.meta_list = meta_list;
	rqd.ppa_list = ppa_list;
	rqd.dma_meta_list = dma_meta_list;
	rqd.dma_ppa_list = dma_ppa_list;
	rqd.opcode = cmd_op;
	rqd.nr_ppas = rq_ppas;

	//根据read 和write不同的方向
	if(dir == WRITE){
		rqd.flags = (geo->plane_mode >> 1) | NVM_IO_SCRAMBLE_ENABLE;
		rqd.ppa_addr = addr_to_gen_ppa(sbi, paddr);
	}else{
		rqd.flags = NVM_IO_SUSPEND | NVM_IO_SCRAMBLE_ENABLE;
		rqd.flags |= geo->plane_mode >> 1;// read sequential	
		rqd.ppa_addr = addr_to_gen_ppa(sbi, paddr);
	}

	ret = nvm_submit_io_sync(dev, &rqd);
	if(ret){
		pr_err("tgt_pblk: emeta I/O submission failed: %d\n", ret);
		bio_put(bio);
		goto free_rqd_dma;
	}
free_rqd_dma:
	nvm_dev_dma_free(dev->parent, rqd.meta_list, rqd.dma_meta_list);
	return ret;
}
*/
//============================================================normal read/write====================


#endif
