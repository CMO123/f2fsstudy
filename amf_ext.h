
#ifndef __AMF_EXT_H
#define __AMF_EXT_H

//一下这3个变量必须与tools/mkfs/f2fs_format.c中一样
#define NR_SUPERBLK_SECS	1	/* # of sections for the super block */
#define NR_MAPPING_SECS		3 	/* # of sections for mapping entries */
#define NR_METALOG_TIMES 	2	/* # of sections for meta-log */

#ifdef AMF_DEBUG_MSG
#define	amf_msg(fmt,...) \
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

struct amf_bio_private{
	struct f2fs_sb_info *sbi;
	bool is_sync;
	void *wait;
	struct page* page;
};

struct amf_map_blk{
	__le32 magic;
	__le32 ver;
	__le32 index;
	__le32 dirty;
	__le32 mapping[F2FS_BLKSIZE/sizeof(__le32)-4];
};

struct amf_info{
	/* meta-log managment */
	int32_t metalog_gc_sblkofs;		/* gc will begin here */
	int32_t metalog_gc_eblkofs;		/* new data will be written here */
	uint32_t metalog_blkofs;		/* the start blkofs of the metalog */
	uint8_t* summary_table;			/* summary table for meta-log */
	uint32_t nr_metalog_logi_blks;	/* # of logical blks for the metalog */
	uint32_t nr_metalog_phys_blks;	/* # of physical blks for the metalog */

	/* mapping  table management */
	int32_t mapping_gc_sblkofs;		/* gc will begin here */
	int32_t mapping_gc_eblkofs;		/* new data will be written here */
	uint32_t mapping_blkofs;		/* the start blkofs of the mapping table */
	struct amf_map_blk* map_blks;	/* mapping table */
	uint32_t nr_mapping_phys_blks;	/* # of physical blks for the mapping table */
	unit32_t nr_mapping_logi_blks;

	/* other variables */
	uint32_t blks_per_sec;
	struct mutext amf_gc_mutex;
#ifdef AMF_DRAM_META_LOGGING
	uint8_t* metalog_dram_buff;
#endif
}

/* some inline functions */
static inline struct amf_info* AMF_RI(struct f2fs_sb_info* sbi){
	return (struct amf_info*)(sbi->amfi);
}

static inline uint32_t SEGS2BLKS (struct f2fs_sb_info* sbi, uint32_t nr_segments){
	return (sbi->blocks_per_seg * nr_segments);
}

//得到meta segments的数目
static inline uint32_t get_nr_logi_meta_segments (struct f2fs_sb_info *sbi){
	struct f2fs_super_block* raw_super = sbi->raw_super;
	return le32_to_cpu(raw_super->segment_count_ckpt) +
		le32_to_cpu(raw_super->segment_count_sit) +
		le32_to_cpu(raw_super->segment_count_nat) +
		le32_to_cpu(raw_super->segment_count_ssa);
}

//逻辑metalog_segments的总数，得到物理meta segments总数，即逻辑meta segment的2倍
static inline uint32_t get_nr_phys_meta_segments (struct f2fs_sb_info *sbi, uint32_t nr_logi_metalog_segments){
	return nr_logi_metalog_segments * NR_METALOG_TIMES;
}


static inline uint32_t get_mapping_blkofs(struct f2fs_sb_info *sbi){
	return sbi->segs_per_sec * sbi->blocks_per_seg;
}

static inline uint32_t get_metalog_blkofs(struct f2fs_sb_info *sbi){
	return (sbi->segs_per_sec * sbi->blocks_per_seg) * (NR_SUPERBLK_SECS + NR_MAPPING_SECS);
}

/* AMF management */
int32_t amf_create_amfi(struct f2fs_sb_info* sbi);
int32_t amf_build_amfi(struct f2fs_sb_info* sbi);
void amf_destory_amfi(struct f2fs_sb_info* sbi);

/* mapping table management */
int32_t amf_write_mapping_entries(struct f2fs_sb_info* sbi);

/* meta-log management */
int32_t is_valid_meta_lblkaddr( struct f2fs_sb_info *sbi, block_t lblkaddr);
int32_t is_valid_meta_pblkaddr(struct f2fs_sb_info* sbi, block_t pblkaddr);
uint32_t amf_get_mapped_pblkaddr( struct f2fs_sb_info* sbi, block_t lblkaddr);
uint32_t amd_get_new_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr, uint32_t length);

int32_t get_metalog_free_blks (struct f2fs_sb_info* sbi);
int8_t amf_map_l2p(struct f2fs_sb_info* sbi, block_t lblkaddr, block_t pblkaddr, uint32_t length);
int8_t is_gc_needed(struct f2fs_sb_info* sbi, int32_t nr_free_blks);
int8_t amf_do_gc(struct f2fs_sb_info* sbi);

int32_t get_mapping_free_blks(struct f2fs_sb_info* sbi);
int8_t is_mapping_gc_needed(struct f2fs_sb_info* sbi, int32_t nr_free_blks);
int8_t amf_do_mapping_gc(struct f2fs_sb_info* sbi);

int8_t amf_do_trim(struct f2fs_sb_info* sbi, block_t pblkaddr, uint32_t nr_blks);
int8_t amf_readpage(struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr);
int8_t amf_writepage(struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr,  uint8_t sync);
void amf_submit_bio(struct f2fs_sb_info* sbi, int rw, struct bio* bio, uint8_t sync);

#ifdef AMF_DRMA_META_LOGGING
int8_t amf_readpage_dram(struct f2fs_sb_info* sbil, struct page* page, block_t pblkaddr);
int8_t amf_writepage_dram(struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr);
void destroy_dram_metalog(struct f2fs_sb_info* sbi);
int8_t build_dram_metalog(struct f2fs_sb_info* sbi);
#endif

#endif