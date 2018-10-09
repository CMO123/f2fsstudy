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

#include <linux/lightnvm.h>


struct lightpblk *light_pblk = NULL;
struct lightpblk {
	struct nvm_tgt_dev *tgt_dev;
	unsigned char *trans_map;
	spinlock_t trans_lock;
};



void* lightpblk_init(struct nvm_tgt_dev *dev, struct gendisk **ptdisk, struct nvm_ioctl_create *create)
{pr_notice("Enter lightpblk_init()\n");

	int ret = 0;	
	struct lightpblk *alightpblk;
	alightpblk= kzalloc(sizeof(struct lightpblk), GFP_KERNEL);
	if (!alightpblk)
		return ERR_PTR(-ENOMEM);
	alightpblk->tgt_dev = dev;
	light_pblk = alightpblk;
	return alightpblk;
	
	
}
void lightpblk_exit(void *private)
{
	pr_notice("Exit lightpblk_exit()\n");
	struct lightpblk* alightpblk = private;
	kfree(alightpblk);
	light_pblk = NULL;	
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
	
	int err;

//1. 组件create参数，创建tgt_dev
	pr_notice(" tgt_bdev->bd_disk->disk_name = %s\n", tgt_bdev->bd_disk->disk_name);//nvme0n1即可
	char dev_name[DISK_NAME_LEN]="";
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


#endif
