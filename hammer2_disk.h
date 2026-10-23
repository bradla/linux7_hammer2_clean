#ifndef _HAMMER2_DISK_H_
#define _HAMMER2_DISK_H_

#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/stddef.h>
#include <linux/compiler.h>

#define HAMMER2_ALLOC_MIN	1024
#define HAMMER2_RADIX_MIN	10
#define HAMMER2_ALLOC_MAX	65536
#define HAMMER2_RADIX_MAX	16
#define HAMMER2_RADIX_KEY	64

#define HAMMER2_SEGSIZE		(1 << HAMMER2_FREEMAP_LEVEL0_RADIX)
#define HAMMER2_SEGRADIX	HAMMER2_FREEMAP_LEVEL0_RADIX

#define HAMMER2_PBUFRADIX	16
#define HAMMER2_PBUFSIZE	65536
#define HAMMER2_LBUFRADIX	14
#define HAMMER2_LBUFSIZE	16384

#define HAMMER2_IND_BYTES_MIN	4096
#define HAMMER2_IND_BYTES_NOM	HAMMER2_LBUFSIZE
#define HAMMER2_IND_BYTES_MAX	HAMMER2_PBUFSIZE
#define HAMMER2_IND_RADIX_MIN	12
#define HAMMER2_IND_RADIX_NOM	HAMMER2_LBUFRADIX
#define HAMMER2_IND_RADIX_MAX	HAMMER2_PBUFRADIX
#define HAMMER2_IND_COUNT_MIN	(HAMMER2_IND_BYTES_MIN / \
				 sizeof(hammer2_blockref_t))
#define HAMMER2_IND_COUNT_NOM	(HAMMER2_IND_BYTES_NOM / \
				 sizeof(hammer2_blockref_t))
#define HAMMER2_IND_COUNT_MAX	(HAMMER2_IND_BYTES_MAX / \
				 sizeof(hammer2_blockref_t))

#define HAMMER2_SET_RADIX		2
#define HAMMER2_SET_COUNT		(1 << HAMMER2_SET_RADIX)
#define HAMMER2_EMBEDDED_BYTES		512
#define HAMMER2_EMBEDDED_RADIX		9

#define HAMMER2_PBUFMASK	(HAMMER2_PBUFSIZE - 1)
#define HAMMER2_LBUFMASK	(HAMMER2_LBUFSIZE - 1)
#define HAMMER2_SEGMASK		(HAMMER2_SEGSIZE - 1)

#define HAMMER2_LBUFMASK64	((u64)HAMMER2_LBUFMASK)
#define HAMMER2_PBUFSIZE64	((u64)HAMMER2_PBUFSIZE)
#define HAMMER2_PBUFMASK64	((u64)HAMMER2_PBUFMASK)
#define HAMMER2_SEGSIZE64	((u64)HAMMER2_SEGSIZE)
#define HAMMER2_SEGMASK64	((u64)HAMMER2_SEGMASK)

#define HAMMER2_UUID_STRING	"5cbb9ad1-862d-11dc-a94d-01301bb8a9f5"

#define HAMMER2_VOLUME_ALIGN		(8 * 1024 * 1024)
#define HAMMER2_VOLUME_ALIGN64		((u64)HAMMER2_VOLUME_ALIGN)
#define HAMMER2_VOLUME_ALIGNMASK	(HAMMER2_VOLUME_ALIGN - 1)
#define HAMMER2_VOLUME_ALIGNMASK64	((u64)HAMMER2_VOLUME_ALIGNMASK)

#define HAMMER2_NEWFS_ALIGN		(HAMMER2_VOLUME_ALIGN)
#define HAMMER2_NEWFS_ALIGN64		((u64)HAMMER2_VOLUME_ALIGN)
#define HAMMER2_NEWFS_ALIGNMASK		(HAMMER2_VOLUME_ALIGN - 1)
#define HAMMER2_NEWFS_ALIGNMASK64	((u64)HAMMER2_NEWFS_ALIGNMASK)

#define HAMMER2_ZONE_BYTES64		(2LLU * 1024 * 1024 * 1024)
#define HAMMER2_ZONE_MASK64		(HAMMER2_ZONE_BYTES64 - 1)
#define HAMMER2_ZONE_SEG		(4 * 1024 * 1024)
#define HAMMER2_ZONE_SEG64		((u64)HAMMER2_ZONE_SEG)
#define HAMMER2_ZONE_BLOCKS_SEG		(HAMMER2_ZONE_SEG / HAMMER2_PBUFSIZE)

#define HAMMER2_ZONE_FREEMAP_INC	5

#define HAMMER2_ZONE_VOLHDR		0
#define HAMMER2_ZONE_FREEMAP_00		1
#define HAMMER2_ZONE_FREEMAP_01		6
#define HAMMER2_ZONE_FREEMAP_02		11
#define HAMMER2_ZONE_FREEMAP_03		16
#define HAMMER2_ZONE_FREEMAP_04		21
#define HAMMER2_ZONE_FREEMAP_05		26
#define HAMMER2_ZONE_FREEMAP_06		31
#define HAMMER2_ZONE_FREEMAP_07		36
#define HAMMER2_ZONE_FREEMAP_END	41

#define HAMMER2_ZONE_END		64

#define HAMMER2_NFREEMAPS		8

#define HAMMER2_ZONEFM_LEVEL1		0
#define HAMMER2_ZONEFM_LEVEL2		1
#define HAMMER2_ZONEFM_LEVEL3		2
#define HAMMER2_ZONEFM_LEVEL4		3
#define HAMMER2_ZONEFM_LEVEL5		4

#define HAMMER2_FREEMAP_LEVEL6_RADIX	64
#define HAMMER2_FREEMAP_LEVEL5_RADIX	62
#define HAMMER2_FREEMAP_LEVEL4_RADIX	54
#define HAMMER2_FREEMAP_LEVEL3_RADIX	46
#define HAMMER2_FREEMAP_LEVEL2_RADIX	38
#define HAMMER2_FREEMAP_LEVEL1_RADIX	30
#define HAMMER2_FREEMAP_LEVEL0_RADIX	22

#define HAMMER2_FREEMAP_LEVELN_PSIZE	32768

#define HAMMER2_FREEMAP_LEVEL5_SIZE	((u64)1 <<					 \
					 HAMMER2_FREEMAP_LEVEL5_RADIX)
#define HAMMER2_FREEMAP_LEVEL4_SIZE	((u64)1 <<					 \
					 HAMMER2_FREEMAP_LEVEL4_RADIX)
#define HAMMER2_FREEMAP_LEVEL3_SIZE	((u64)1 <<					 \
					 HAMMER2_FREEMAP_LEVEL3_RADIX)
#define HAMMER2_FREEMAP_LEVEL2_SIZE	((u64)1 <<					 \
					 HAMMER2_FREEMAP_LEVEL2_RADIX)
#define HAMMER2_FREEMAP_LEVEL1_SIZE	((u64)1 <<					 \
					 HAMMER2_FREEMAP_LEVEL1_RADIX)
#define HAMMER2_FREEMAP_LEVEL0_SIZE	((u64)1 <<					 \
					 HAMMER2_FREEMAP_LEVEL0_RADIX)

#define HAMMER2_FREEMAP_LEVEL5_MASK	(HAMMER2_FREEMAP_LEVEL5_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL4_MASK	(HAMMER2_FREEMAP_LEVEL4_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL3_MASK	(HAMMER2_FREEMAP_LEVEL3_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL2_MASK	(HAMMER2_FREEMAP_LEVEL2_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL1_MASK	(HAMMER2_FREEMAP_LEVEL1_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL0_MASK	(HAMMER2_FREEMAP_LEVEL0_SIZE - 1)

#define HAMMER2_FREEMAP_COUNT		(int)(HAMMER2_FREEMAP_LEVELN_PSIZE / \
					 sizeof(hammer2_bmap_data_t))

#define H2FMZONEBASE(key)	((key) & ~HAMMER2_FREEMAP_LEVEL1_MASK)
#define H2FMBASE(key, radix)	round_down(key, (u64)1 << (radix))

#define HAMMER2_FREEMAP_BLOCK_RADIX	14
#define HAMMER2_FREEMAP_BLOCK_SIZE	(1 << HAMMER2_FREEMAP_BLOCK_RADIX)
#define HAMMER2_FREEMAP_BLOCK_MASK	(HAMMER2_FREEMAP_BLOCK_SIZE - 1)

typedef u64 hammer2_bitmap_t;

#define HAMMER2_BMAP_ALLONES		((hammer2_bitmap_t)-1)
#define HAMMER2_BMAP_ELEMENTS		8
#define HAMMER2_BMAP_BITS_PER_ELEMENT	64
#define HAMMER2_BMAP_INDEX_RADIX	5
#define HAMMER2_BMAP_BLOCKS_PER_ELEMENT	(1 << HAMMER2_BMAP_INDEX_RADIX)

#define HAMMER2_BMAP_INDEX_SIZE		(HAMMER2_FREEMAP_BLOCK_SIZE * \
					 HAMMER2_BMAP_BLOCKS_PER_ELEMENT)
#define HAMMER2_BMAP_INDEX_MASK		(HAMMER2_BMAP_INDEX_SIZE - 1)

#define HAMMER2_BMAP_SIZE		(HAMMER2_BMAP_INDEX_SIZE * \
					 HAMMER2_BMAP_ELEMENTS)
#define HAMMER2_BMAP_MASK		(HAMMER2_BMAP_SIZE - 1)

#define HAMMER2_BOOT_MIN_BYTES		HAMMER2_VOLUME_ALIGN
#define HAMMER2_BOOT_NOM_BYTES		(64*1024*1024)
#define HAMMER2_BOOT_MAX_BYTES		(256*1024*1024)

#define HAMMER2_AUX_MIN_BYTES		HAMMER2_VOLUME_ALIGN
#define HAMMER2_AUX_NOM_BYTES		(256*1024*1024)
#define HAMMER2_AUX_MAX_BYTES		(1024*1024*1024)

typedef u64 hammer2_tid_t;
typedef u64 hammer2_off_t;
typedef u64 hammer2_key_t;
typedef u32 hammer2_crc32_t;

#define HAMMER2_TID_MIN		1ULL
#define HAMMER2_TID_MAX		0xFFFFFFFFFFFFFFFFULL
#define HAMMER2_KEY_MIN		0ULL
#define HAMMER2_KEY_MAX		0xFFFFFFFFFFFFFFFFULL

#define HAMMER2_OFF_MASK	0xFFFFFFFFFFFFFFC0ULL
#define HAMMER2_OFF_MASK_LO	(HAMMER2_OFF_MASK & HAMMER2_PBUFMASK64)
#define HAMMER2_OFF_MASK_HI	(~HAMMER2_PBUFMASK64)
#define HAMMER2_OFF_MASK_RADIX	0x000000000000003FULL

#define HAMMER2_DIRHASH_VISIBLE	0x8000000000000000ULL
#define HAMMER2_DIRHASH_USERMSK	0x7FFFFFFFFFFFFFFFULL
#define HAMMER2_DIRHASH_LOMASK	0x0000000000007FFFULL

#define HAMMER2_SROOT_KEY	0x0000000000000000ULL
#define HAMMER2_BOOT_KEY	0xd9b36ce135528000ULL

struct hammer2_dirent_head {
	hammer2_tid_t		inum;
	u16			namlen;
	u8			type;
	u8			unused0B;
	u8			unused0C[4];
} __packed;

typedef struct hammer2_dirent_head hammer2_dirent_head_t;

struct hammer2_blockref {
	u8		type;
	u8		methods;
	u8		copyid;
	u8		keybits;
	u8		vradix;
	u8		flags;
	u16		leaf_count;
	hammer2_key_t	key;
	hammer2_tid_t	mirror_tid;
	hammer2_tid_t	modify_tid;
	hammer2_off_t	data_off;
	hammer2_tid_t	update_tid;
	union {
		char	buf[16];

		hammer2_dirent_head_t dirent;

		struct {
			hammer2_key_t	data_count;
			hammer2_key_t	inode_count;
		} stats;
	} embed;
	union {
		char	buf[64];
		struct {
			u32 value;
			u32 reserved[15];
		} iscsi32;
		struct {
			u64 value;
			u64 reserved[7];
		} xxhash64;
		struct {
			char data[24];
			char reserved[40];
		} sha192;
		struct {
			char data[32];
			char reserved[32];
		} sha256;
		struct {
			char data[64];
		} sha512;

		struct {
			u32 icrc32;
			u32 bigmask;
			u64 avail;
			char reserved[48];
		} freemap;
	} check;
} __packed;

typedef struct hammer2_blockref hammer2_blockref_t;

#define HAMMER2_BLOCKREF_BYTES		128
#define HAMMER2_BLOCKREF_RADIX		7

#define HAMMER2_BLOCKREF_LEAF_MAX	65535

#define HAMMER2_BREF_TYPE_EMPTY		0
#define HAMMER2_BREF_TYPE_INODE		1
#define HAMMER2_BREF_TYPE_INDIRECT	2
#define HAMMER2_BREF_TYPE_DATA		3
#define HAMMER2_BREF_TYPE_DIRENT	4
#define HAMMER2_BREF_TYPE_FREEMAP_NODE	5
#define HAMMER2_BREF_TYPE_FREEMAP_LEAF	6
#define HAMMER2_BREF_TYPE_INVALID	7
#define HAMMER2_BREF_TYPE_FREEMAP	254
#define HAMMER2_BREF_TYPE_VOLUME	255

#define HAMMER2_BREF_FLAG_PFSROOT	0x01
#define HAMMER2_BREF_FLAG_EMERG_MIP	0x04

#define HAMMER2_CHECK_NONE		0
#define HAMMER2_CHECK_DISABLED		1
#define HAMMER2_CHECK_ISCSI32		2
#define HAMMER2_CHECK_XXHASH64		3
#define HAMMER2_CHECK_SHA192		4
#define HAMMER2_CHECK_FREEMAP		5

#define HAMMER2_CHECK_DEFAULT		HAMMER2_CHECK_XXHASH64

#define HAMMER2_COMP_NONE		0
#define HAMMER2_COMP_AUTOZERO		1
#define HAMMER2_COMP_LZ4		2
#define HAMMER2_COMP_ZLIB		3

#define HAMMER2_COMP_DEFAULT		HAMMER2_COMP_LZ4

#define HAMMER2_ENC_CHECK(n)		(((n) & 15) << 4)
#define HAMMER2_DEC_CHECK(n)		(((n) >> 4) & 15)
#define HAMMER2_ENC_COMP(n)		((n) & 15)
#define HAMMER2_DEC_COMP(n)		((n) & 15)

#define HAMMER2_ENC_ALGO(n)		(n)
#define HAMMER2_DEC_ALGO(n)		((n) & 15)
#define HAMMER2_ENC_LEVEL(n)		((n) << 4)
#define HAMMER2_DEC_LEVEL(n)		(((n) >> 4) & 15)

struct hammer2_blockset {
	hammer2_blockref_t	blockref[HAMMER2_SET_COUNT];
};

typedef struct hammer2_blockset hammer2_blockset_t;

struct hammer2_bmap_data {
	s32 linear;
	u16 class;
	u8 reserved06;
	u8 reserved07;
	u32 reserved08;
	u32 reserved0C;
	u32 reserved10;
	u32 reserved14;
	u32 reserved18;
	u32 avail;
	u32 reserved20[8];

	hammer2_bitmap_t bitmapq[HAMMER2_BMAP_ELEMENTS];
} __packed;

typedef struct hammer2_bmap_data hammer2_bmap_data_t;

#define HAMMER2_INODE_BYTES		1024
#define HAMMER2_INODE_MAXNAME		256
#define HAMMER2_INODE_VERSION_ONE	1

#define HAMMER2_INODE_START		1024

#define HAMMER2_LINK_MAX		UINT32_MAX

struct hammer2_inode_meta {
	u16		version;
	u8		reserved02;
	u8		pfs_subtype;

	u32		uflags;
	u32		rmajor;
	u32		rminor;
	u64		ctime;
	u64		mtime;
	u64		atime;
	u64		btime;
	struct uuid	uid;
	struct uuid	gid;

	u8		type;
	u8		op_flags;
	u16		cap_flags;
	u32		mode;

	hammer2_tid_t	inum;
	hammer2_off_t	size;
	u64		nlinks;
	hammer2_tid_t	iparent;
	hammer2_key_t	name_key;
	u16		name_len;
	u8		ncopies;
	u8		comp_algo;
	u8		unused84;
	u8		check_algo;

	u8		pfs_nmasters;
	u8		pfs_type;
	hammer2_tid_t	pfs_inum;
	struct uuid	pfs_clid;
	struct uuid	pfs_fsid;

	hammer2_key_t	data_quota;
	hammer2_key_t	unusedB8;
	hammer2_key_t	inode_quota;
	hammer2_key_t	unusedC8;

	hammer2_tid_t	pfs_lsnap_tid;
	hammer2_tid_t	reservedD8;

	u64		decrypt_check;
	hammer2_off_t	reservedE8[3];
} __packed;

typedef struct hammer2_inode_meta hammer2_inode_meta_t;

struct hammer2_inode_data {
	hammer2_inode_meta_t	meta;
	unsigned char	filename[HAMMER2_INODE_MAXNAME];

	union {
		hammer2_blockset_t blockset;
		char data[HAMMER2_EMBEDDED_BYTES];
	} u;
} __packed;

typedef struct hammer2_inode_data hammer2_inode_data_t;

#define HAMMER2_OPFLAG_DIRECTDATA	0x01
#define HAMMER2_OPFLAG_PFSROOT		0x02

#define HAMMER2_OBJTYPE_UNKNOWN		0
#define HAMMER2_OBJTYPE_DIRECTORY	1
#define HAMMER2_OBJTYPE_REGFILE		2
#define HAMMER2_OBJTYPE_FIFO		4
#define HAMMER2_OBJTYPE_CDEV		5
#define HAMMER2_OBJTYPE_BDEV		6
#define HAMMER2_OBJTYPE_SOFTLINK	7
#define HAMMER2_OBJTYPE_UNUSED08	8
#define HAMMER2_OBJTYPE_SOCKET		9
#define HAMMER2_OBJTYPE_WHITEOUT	10

#define HAMMER2_COPYID_LOCAL		((u8)-1)

#define HAMMER2_PFSTYPE_NONE		0x00
#define HAMMER2_PFSTYPE_CACHE		0x01
#define HAMMER2_PFSTYPE_SLAVE		0x03
#define HAMMER2_PFSTYPE_SOFT_SLAVE	0x04
#define HAMMER2_PFSTYPE_SOFT_MASTER	0x05
#define HAMMER2_PFSTYPE_MASTER		0x06
#define HAMMER2_PFSTYPE_SUPROOT		0x08
#define HAMMER2_PFSTYPE_DUMMY		0x09
#define HAMMER2_PFSTYPE_MAX		16

#define HAMMER2_PFSSUBTYPE_NONE		0
#define HAMMER2_PFSSUBTYPE_SNAPSHOT	1
#define HAMMER2_PFSSUBTYPE_AUTOSNAP	2

#define HAMMER2_VOLUME_ID_HBO	0x48414d3205172011LLU
#define HAMMER2_VOLUME_ID_ABO	0x11201705324d4148LLU

#define HAMMER2_MAX_VOLUMES	64
#define HAMMER2_ROOT_VOLUME	0

struct hammer2_volume_data {

	u64		magic;
	hammer2_off_t	boot_beg;
	hammer2_off_t	boot_end;
	hammer2_off_t	aux_beg;
	hammer2_off_t	aux_end;
	hammer2_off_t	volu_size;

	u32		version;
	u32		flags;
	u8		copyid;
	u8		freemap_version;
	u8		peer_type;
	u8		volu_id;
	u8		nvolumes;
	u8		reserved003D;
	u16		reserved003E;

	struct uuid	fsid;
	struct uuid	fstype;

	hammer2_off_t	allocator_size;
	hammer2_off_t   allocator_free;
	hammer2_off_t	allocator_beg;

	hammer2_tid_t	mirror_tid;
	hammer2_tid_t	reserved0080;
	hammer2_tid_t	reserved0088;
	hammer2_tid_t	freemap_tid;
	hammer2_tid_t	bulkfree_tid;
	hammer2_tid_t	reserved00A0[4];

	hammer2_off_t	total_size;

	u32		copyexists[8];
	char		reserved00E8[248];

	hammer2_crc32_t	icrc_sects[8];

	hammer2_blockset_t sroot_blockset;

	char	sector2[512];
	char	sector3[512];
	hammer2_blockset_t freemap_blockset;
	char	sector5[512];
	char	sector6[512];

	hammer2_off_t volu_loff[HAMMER2_MAX_VOLUMES];

	char		reserved_volconf[0x8000];

	char		reserved9000[0x6FFC];

	hammer2_crc32_t	icrc_volheader;
} __packed;

typedef struct hammer2_volume_data hammer2_volume_data_t;

#define HAMMER2_VOL_ICRC_SECT0		7
#define HAMMER2_VOL_ICRC_SECT1		6

#define HAMMER2_VOLUME_BYTES		65536

#define HAMMER2_VOLUME_ICRC0_OFF	0
#define HAMMER2_VOLUME_ICRC1_OFF	512
#define HAMMER2_VOLUME_ICRCVH_OFF	0

#define HAMMER2_VOLUME_ICRC0_SIZE	(512 - 4)
#define HAMMER2_VOLUME_ICRC1_SIZE	(512)
#define HAMMER2_VOLUME_ICRCVH_SIZE	(65536 - 4)

#define HAMMER2_VOL_VERSION_MULTI_VOLUMES	2

#define HAMMER2_VOL_VERSION_MIN		1
#define HAMMER2_VOL_VERSION_DEFAULT	HAMMER2_VOL_VERSION_MULTI_VOLUMES
#define HAMMER2_VOL_VERSION_WIP		(HAMMER2_VOL_VERSION_MULTI_VOLUMES + 1)

#define HAMMER2_NUM_VOLHDRS		4

union hammer2_media_data {
	hammer2_volume_data_t	voldata;
        hammer2_inode_data_t    ipdata;
	hammer2_blockset_t	blkset;
	hammer2_blockref_t	npdata[HAMMER2_IND_COUNT_MAX];
	hammer2_bmap_data_t	bmdata[HAMMER2_FREEMAP_COUNT];
	char			buf[HAMMER2_PBUFSIZE];
} __packed;

typedef union hammer2_media_data hammer2_media_data_t;

#define HAMMER2_BUILD_BUG_ON(cond) BUILD_BUG_ON(cond)

#if 0
HAMMER2_BUILD_BUG_ON((1 << HAMMER2_SET_RADIX) != HAMMER2_SET_COUNT);
HAMMER2_BUILD_BUG_ON((1 << HAMMER2_PBUFRADIX) != HAMMER2_PBUFSIZE);
HAMMER2_BUILD_BUG_ON((1 << HAMMER2_RADIX_MIN) != HAMMER2_ALLOC_MIN);
HAMMER2_BUILD_BUG_ON(sizeof(struct hammer2_blockref) != HAMMER2_BLOCKREF_BYTES);
HAMMER2_BUILD_BUG_ON(sizeof(struct hammer2_inode_data) != HAMMER2_INODE_BYTES);
HAMMER2_BUILD_BUG_ON(sizeof(struct hammer2_volume_data) != HAMMER2_VOLUME_BYTES);
#endif

#endif
